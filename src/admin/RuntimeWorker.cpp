#include "firefly/admin/RuntimeWorker.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/llm/StructuredOutput.h"
#include "Json.h"
#include "AdminBuild.h"
#include "DialogueOperations.h"
#include "firefly/admin/AdminRequestPolicy.h"
#include "../Text.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/actions/ScalarJson.h"
#include <iostream>
#include <algorithm>
#include <thread>
#include <unistd.h>

namespace firefly::admin {
namespace {
bool hasUnexcludedBrightness(const std::string& text, const std::vector<std::string>& words) {
    std::size_t previousEnd = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto begin = text.find(words[i], previousEnd);
        if (begin == std::string::npos) return true;
        if (words[i].find("ярк") != std::string::npos) {
            // Only the complete phrase "без яркости", separated by whitespace,
            // excludes brightness. Do not carry negation across punctuation or
            // hide another affirmative/ambiguous brightness mention.
            const bool excluded = words[i] == "яркости" && i > 0 && words[i - 1] == "без" &&
                text.find_first_not_of(" \t\r\n", previousEnd) == begin;
            if (!excluded) return true;
        }
        previousEnd = begin + words[i].size();
    }
    return false;
}
dialogue::Facts dialogueFacts(const RuntimeGeneration& generation, const std::vector<ExecutionEvent>& history, bool modelReady) {
    dialogue::Facts facts{generation.domain.snapshot(), generation.id, generation.executor->production().steadyNowMs(), generation.executor->production().devices(), {}};
    facts.modelReady = modelReady;
    for (const auto& event : history) facts.history.push_back({event.sequence, event.generation, event.source, event.result});
    return facts;
}
// One bounded observation window under the already admitted generation request.
// MQTT reads only the existing subscription store; no request/PUBLISH is invented.
// HTTP uses the existing validated GET with a stricter remaining deadline.
dialogue::Facts voiceReadback(const RuntimeGeneration& generation, dialogue::Facts facts,
                             const std::vector<std::string>& targets, const std::function<bool()>& stopped,
                             const execution::ExecutionResult* receipt = nullptr,
                             const actions::GenericAction* command = nullptr, std::int64_t after = 0) {
    using Clock = std::chrono::steady_clock;
    const auto deadline=Clock::now()+std::chrono::milliseconds(500);
    const auto update=[&] {
        facts.devices=generation.executor->production().devices();
        facts.steadyMs=generation.executor->production().steadyNowMs();
    };
    const auto known=[&](const std::string& target) {
        if(receipt && command)return dialogue::OperationalDialogue::executionConfirmed(facts,*receipt,*command,after);
        for(const auto& d:facts.devices)if(d.targetId==target)for(const auto& [field,p]:d.properties) {
            (void)field;
            if(p.value&&p.knowledge==execution::StateKnowledge::Confirmed&&p.freshnessSteadyMs&&
               *p.freshnessSteadyMs<=facts.steadyMs&&!execution::isStale(p,facts.steadyMs))return true;
        }
        return false;
    };
    for(const auto& target:targets) {
        if(stopped())break;
        update();if(receipt&&known(target))break;
        const auto& bindings=facts.catalog->configuration().observations;
        const auto binding=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.targetId==target;});
        if(binding==bindings.end())continue;
        if(binding->executor==config::ExecutorKind::Http) {
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if(remaining<=0){facts.failedReads.push_back(target);continue;}
            const auto read=generation.executor->poll(target,remaining);
            if(!read.success)facts.failedReads.push_back(target);
            std::cerr << ("VOICE_READBACK " + Json{{"target_id",target},{"transport","http"},{"ok",read.success},{"error",read.error}}.dump()+"\n");
        } else if(binding->executor==config::ExecutorKind::Mqtt) {
            while(!known(target)&&Clock::now()<deadline&&!stopped()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));update();
            }
            std::cerr << ("VOICE_READBACK " + Json{{"target_id",target},{"transport","mqtt_state"},{"known",known(target)},{"publish",false}}.dump()+"\n");
        }
    }
    update();return facts;
}
Json stateJson(const execution::DeviceState& state) {
    if (const auto* fields = std::get_if<execution::TypedState>(&state)) {
        Json result = Json::object();
        for (const auto& [name, value] : *fields) result[name] = actions::scalarJson(value);
        return result;
    }
    if (const auto* light = std::get_if<execution::LightState>(&state)) return {{"brightness", light->brightness}};
    if (std::holds_alternative<execution::InventoryState>(state)) return {{"status", "unconfigured"}};
    return {{"state", std::get<execution::DoorState>(state).position == execution::DoorPosition::Open ? "OPEN" : "CLOSED"}};
}
Json propertyKnowledgeJson(const execution::PropertyKnowledge& property, std::int64_t nowMs) {
    Json value = nullptr;
    if (property.value) value = actions::scalarJson(*property.value);
    return {{"value", value},
        {"knowledge", execution::DeviceStateStore::knowledgeName(property.knowledge)},
        {"stale", execution::isStale(property, nowMs)},
        {"updated_at_ms", property.updatedAtMs},
        {"last_observed_value", property.lastObservedValue ? actions::scalarJson(*property.lastObservedValue) : Json(nullptr)},
        {"observation_age_ms", property.lastObservationSteadyMs && *property.lastObservationSteadyMs <= nowMs ? Json(nowMs - *property.lastObservationSteadyMs) : Json(nullptr)},
        {"observed_at_ms", property.observedAtMs > 0 ? Json(property.observedAtMs) : Json(nullptr)}};
}
Json deviceKnowledgeJson(const execution::DeviceKnowledge& device, std::int64_t nowMs) {
    Json properties = Json::object();
    Json legacy = Json::object();
    for (const auto& [key, property] : device.properties) {
        properties[key] = propertyKnowledgeJson(property, nowMs);
        if (property.value) {
            if (key == "position") legacy["state"] = actions::scalarJson(*property.value);
            else legacy[key] = actions::scalarJson(*property.value);
        }
    }
    if (device.properties.empty()) legacy = {{"status", "unconfigured"}};
    return {{"targetId", device.targetId}, {"name", device.name},
        {"properties", properties},
        {"state", legacy},
        {"availability", execution::availability(device, nowMs)},
        {"availability_reason", device.observationDisconnected ? "observation_disconnected" :
            !execution::observationAge(device, nowMs) ? "no_observation" :
            *execution::observationAge(device, nowMs) > execution::kDefaultStateTtlMs ? "telemetry_timeout" : "recent_observation"},
        {"last_observation_at_ms", device.lastObservationAtMs > 0 ? Json(device.lastObservationAtMs) : Json(nullptr)},
        {"observation_age_ms", execution::observationAge(device, nowMs) ? Json(*execution::observationAge(device, nowMs)) : Json(nullptr)},
        {"connectivity", execution::DeviceStateStore::connectivityName(device.connectivity)},
        {"last_success_at_ms", device.lastSuccessAtMs > 0 ? Json(device.lastSuccessAtMs) : Json(nullptr)},
        {"last_failure_at_ms", device.lastFailureAtMs > 0 ? Json(device.lastFailureAtMs) : Json(nullptr)}};
}
Json executionJson(const execution::ExecutionResult& result) {
    return {{"success", result.success},
        {"status", result.status == execution::ExecutionStatus::Accepted ? "accepted" : "failed"},
        {"actionId", result.actionId}, {"targetId", result.targetId},
        {"previous", result.previous ? stateJson(*result.previous) : Json(nullptr)},
        {"current", result.current ? stateJson(*result.current) : Json(nullptr)},
        {"error", result.error}, {"timestamp_ms", result.timestampMs}};
}
Json commandJson(const std::string& input, const GenerationResult& result) {
    const auto& command = result.command;
    Json reply = {{"ok", command.error.empty() || command.domainRejected || command.path == CommandPath::GroundingRejected}, {"input", input},
        {"route", command.path == CommandPath::FastPath ? "deterministic" :
            command.path == CommandPath::LlmFallback ? "rkllm" : command.path == CommandPath::GroundingRejected ? "grounding_rejected" : "input_rejected"},
        {"result", Json::parse(llm::structuredResult(command.action))},
        {"candidate", Json::parse(llm::structuredResult(command.candidate))},
        {"safety_result", Json::parse(llm::structuredResult(command.safeCandidate))},
        {"decode", llm::decodeStatusName(command.decodeStatus)},
        {"safety", command.safetyRejected ? "REJECT" :
            std::holds_alternative<actions::GenericAction>(command.safeCandidate) ? "PASS" : "NO_ACTION"},
        {"domain", command.domainRejected || command.decodeStatus == llm::DecodeStatus::OutsideDomain ? "REJECT" :
            std::holds_alternative<actions::GenericAction>(command.action) ? "PASS" : "NO_ACTION"},
        {"llm", {{"context_ms",command.llmTimings.contextMs},{"prefill_ms",command.llmTimings.prefillMs},
            {"generation_ms",command.llmTimings.generationMs},{"output_tokens",command.llmTimings.outputTokens},
            {"invoked",command.llmTimings.invoked},{"task",command.llmTimings.task}}},
        {"latency_ms", command.latencyMs}, {"revision", result.generation->domain.snapshot()->configuration().revision},
        {"hash", result.generation->domain.snapshot()->contentHash()}};
    if (command.path == CommandPath::LlmFallback && result.guardError.empty()) {
        reply["raw_rkllm"] = command.rawOutput;
        reply["llm_context"] = result.generation->requestPrompt(input);
    }
    if (!command.error.empty() && !command.domainRejected && command.path != CommandPath::GroundingRejected) {
        std::cerr << "Admin test runtime: " << command.error << '\n';
        reply["errors"] = failure(command.path == CommandPath::InputRejected ? "invalid_input" : "command_failed", "/text", command.error)["errors"];
    }
    reply["matching_evidence"] = Json::array();
    for (const auto& m : command.grounding.mentions) reply["matching_evidence"].push_back({
        {"entity_id",m.entityId},{"category",m.category},{"alias",m.alias},{"span",m.span},{"method",m.method},
        {"begin",m.begin},{"end",m.end},{"distance",m.distance},{"generation_id",result.generation->id},{"catalog_hash",command.grounding.catalogHash}});
    reply["language_route"]=command.languageRoute;reply["index_status"]=command.indexStatus;
    reply["index_match"]={{"stage",command.indexMatch.stage},{"reason",command.indexMatch.reason},
        {"target_id",command.indexMatch.targetId},{"action_id",command.indexMatch.actionId},{"uncovered",command.indexMatch.uncovered}};
    reply["language_index"]={{"hash",result.generation->languageIndex->catalogHash()},{"revision",result.generation->languageIndex->revision()},
        {"compiler",language::LanguageIndex::CompilerVersion},{"fingerprint",result.generation->languageIndex->fingerprint()}};
    reply["ambiguous"] = command.grounding.ambiguous;
    reply["reason"] = command.reason;
    reply["rejection_stage"] = command.path == CommandPath::GroundingRejected ? "grounding" : command.safetyRejected ? "safety" : command.domainRejected || command.decodeStatus == llm::DecodeStatus::OutsideDomain ? "domain" :
        command.invalidModelOutput ? "decoder" : command.decodeStatus == llm::DecodeStatus::Unsupported ? "model_unsupported" :
        !command.error.empty() ? "runtime" : "";
    reply["generation_id"] = result.generation->id;
    reply["execution"] = result.execution ? executionJson(*result.execution) : Json(nullptr);
    if (!result.guardError.empty()) {
        reply["ok"] = false;
        reply["errors"] = failure(result.guardError, "/expected_action", "Guarded command rejected before execution; no hardware request sent.")["errors"];
        reply["rejection_stage"] = "command_guard";
    }
    if (result.execution && !result.execution->success) reply["rejection_stage"] = "executor";
    reply["predicted_execution"] = nullptr;
    if (!result.execution) if (const auto* action = std::get_if<actions::GenericAction>(&command.action)) {
        reply["predicted_execution"] = executionJson(result.generation->executor->preview(*action));
        reply["predicted_execution"]["executed"] = false;
    }
    return reply;
}
}

std::string RuntimeWorker::handle(std::string_view bytes) {
    Json reply;
    try {
        const auto request = parseMessage(bytes);
        const auto method = request.at("method").get<std::string>();
        if (method == "cancel_speech") {
            members(request, {"method"}); cancelSpeech(); reply = {{"ok", true}};
        } else if (method == "setup_enter" || method == "setup_restore" || method == "setup_finish") {
            members(request,{"method"});
            if (method == "cancel_speech") {
            members(request, {"method"}); cancelSpeech(); reply = {{"ok", true}};
        } else if (method == "setup_enter") generations_.enterSetup();
            else if (method == "setup_restore") generations_.prepareRestore();
            else generations_.finishRestore();
            reply = {{"ok",true}};
        } else if (method == "apply" || method == "preflight") {
            members(request, {"method", "configuration"});
            auto catalog = config::ConfigLoader::fromJson(request.at("configuration").dump());
            if (method == "apply") generations_.apply(std::move(catalog));
            else generations_.preflight(std::move(catalog));
            reply = {{"ok", true}};
        } else if (method == "state_diff") {
            members(request,{"method","configuration"});
            const auto next = config::ConfigLoader::fromJson(request.at("configuration").dump());
            execution::MockDeviceExecutor preview(next);
            Json changes = Json::array();std::vector<execution::DeviceRecord> before;
            if (const auto active = generations_.current()) before = active->executor->states();
            try {
                preview.reconcile(before);
                const auto after = preview.states();
                for (const auto& old : before) {
                    const auto found = std::find_if(after.begin(),after.end(),[&](const auto& d){return d.targetId==old.targetId;});
                    if (found == after.end()) changes.push_back({{"entity","state"},{"name",old.name},{"change","changed"},{"before",{{"state",stateJson(old.state)}}},{"after",{{"state","Устройство удаляется"}}}});
                    else if (!(found->state == old.state)) changes.push_back({{"entity","state"},{"name",found->name},{"change","changed"},{"before",{{"state",stateJson(old.state)}}},{"after",{{"state",stateJson(found->state)}}}});
                }
                for (const auto& added : after) if (std::none_of(before.begin(),before.end(),[&](const auto& d){return d.targetId==added.targetId;}))
                    changes.push_back({{"entity","state"},{"name",added.name},{"change","changed"},{"before",{{"state",nullptr}}},{"after",{{"state",stateJson(added.state)}}}});
                reply = {{"ok",true},{"compatible",true},{"changes",changes},{"policy","preserve_or_reject; initialize_new_or_inventory; remove_deleted"}};
            } catch (const config::ConfigError& error) {
                changes.push_back({{"entity","state"},{"name","Apply будет отклонён"},{"change","changed"},{"before",{{"state","Рабочее состояние сохраняется"}}},{"after",{{"state",error.what()}}}});
                reply = {{"ok",true},{"compatible",false},{"changes",changes},{"error",error.what()}};
            }
        } else if (method == "dialogue") {
            auto fields=std::set<std::string>{"method","configuration","text","focus"};
            if(request.contains("context_location"))fields.insert("context_location");
            members(request,fields);
            const auto input = request.at("text").get<std::string>();
            validateMessage(input);
            const auto catalog = config::ConfigLoader::fromJson(request.at("configuration").dump());
            std::string prompt=R"(Помощник настройки системы и оборудования. Выбери одну ветвь ответа и сформируй один JSON-документ с одним полем.
Если просьба выполнима, поле ops содержит массив операций. Если необходим ответ пользователя, единственное поле question содержит конкретный вопрос строкой. Если просьба не поддерживается, единственное поле unsupported содержит причину строкой. Эти ветви взаимоисключающие: выбери ровно одну.
В ops каждая операция — массив в квадратных скобках: ["location","имя зоны"]. Фигурные скобки нужны только внешнему объекту, не операциям.
Пример: «Создай зону Павильон и обогреватель с включением и выключением.» -> {"ops":[["location","Павильон"],["power_device","Павильон","Обогреватель"]]}
Это пример формы. Используй только названия из текущей просьбы. Создание зоны не означает переименование системы; system_name допустим только при явной просьбе назвать или переименовать систему.
Не более 3 операций. Строгие форматы массивов:
["system_name","новое название системы"] — только явно запрошенное имя.
["location","название новой зоны"]
["power_device","зона","название устройства"] — ТОЛЬКО включение/выключение, без яркости, громкости и каналов.
["device","зона","название устройства"] — без запрошенных возможностей. Вместо зоны null допустим только при явном «без зоны».
["rename_device","зона","старое название","новое название"]
["rename_location","старое название","новое название",true] — последний элемент true сохраняет старое имя как alias, false не сохраняет.
["delete_device","зона","устройство"]
["add_brightness","зона","устройство"] — только явно запрошенная яркость 0..100.
["bounds","зона","устройство","поле",0,100] — последние два числа только из явно запрошенных границ; 0 и 100 здесь пример, не значения по умолчанию.
Названия — на языке пользователя, не транслитерация и не IDs. Сначала зона, затем её устройства. Описание наличия нового устройства и его возможностей — просьба добавить его. Местоимение может относиться к единственному явно названному в этой же просьбе новому устройству. Если цель неоднозначна, верни question. Не выдумывай возможности и подключения. Не пропускай ограничения. Неподдерживаемая просьба целиком -> unsupported. Все операции внутри одного ops; после единственного JSON остановись. Без markdown и пояснений.)";
            const auto normalized = detail::lowercase(input);
            const auto words=detail::words(normalized);
            const bool creationRequest=(normalized.find("созда")!=std::string::npos || normalized.find("добав")!=std::string::npos ||
                std::find(words.begin(),words.end(),"есть")!=words.end()) &&
                normalized.find("назови")==std::string::npos &&
                normalized.find("удали")==std::string::npos && normalized.find("убери")==std::string::npos &&
                normalized.find("измени")==std::string::npos && normalized.find("диапаз")==std::string::npos &&
                normalized.find("переимен")==std::string::npos && (" "+normalized+" ").find(" не ")==std::string::npos;
            // Instruction selection alone recognizes explicit exclusion. Keep
            // existing scope and ambiguity rules independent of this correction.
            const bool creationOnly=creationRequest && normalized.find("ярк")==std::string::npos;
            const bool creationPrompt=creationOnly || (creationRequest &&
                std::find(words.begin(),words.end(),"не")==words.end() &&
                !hasUnexcludedBrightness(normalized,words));
            if(creationPrompt)prompt=R"(Помощник настройки системы и оборудования. Разбери просьбу о создании. Не выполняй её. Выбери одну ветвь ответа и сформируй один JSON-документ с одним полем.
Успешный план содержит только поле ops с массивом операций. Если необходимо уточнение, только поле question с вопросом строкой. Если просьба не поддерживается, только поле unsupported с причиной строкой. Эти ветви взаимоисключающие: выбери ровно одну.
В ops каждая операция — массив в квадратных скобках. Фигурные скобки нужны только внешнему объекту, не операциям.
Это пример формы, не готовые названия. В ops от 1 до 3 операций, только запрошенные. Сначала новая зона, затем её устройства. Если зона уже существует, операция location не нужна.
Форматы операций: ["location","имя зоны"], ["power_device","имя зоны","имя устройства"], ["device","имя зоны","имя устройства"]. При явном «без зоны» второй элемент power_device или device — null вместо строки.
В power_device второй элемент — место размещения, третий — название предмета. Включить/выключить — возможности этого предмета, а не имена отдельных устройств.
Пример при пустом DATA: «В павильоне есть обогреватель, его можно включать и выключать.» -> {"ops":[["location","Павильон"],["power_device","Павильон","Обогреватель"]]}
power_device означает ровно включение и выключение питания. Обороты «может включаться и выключаться», «можно включать и выключать» описывают эти же возможности, а не дополнительные параметры. device — устройство без запрошенных возможностей.
Описание наличия нового устройства и его возможностей — просьба добавить устройство в указанную зону; если она новая, сначала добавь зону. Местоимение может относиться к единственному явно названному в этой же просьбе устройству.
Используй названное пользователем назначение или описание помещения как его имя в именительном падеже. Отдельное имя не требуется: НЕ спрашивай как назвать новую зону. Сохраняй слова названия из просьбы: разрешены только изменение падежа и регистра, НЕ замена синонимами. Названия по-русски, не IDs и не транслитерация. Заменяй обозначения полей реальными именами из просьбы.
Место, цех, склад, лабораторный стенд — необязательная зона. DATA содержит УЖЕ существующие зоны: НЕ создавай их повторно. Создавай только то, что прямо запрошено текущей просьбой. Новая запрошенная зона может отсутствовать в DATA. Ссылка там означает текущую зону; сервер проверит ссылки.
Для явно указанного «без зоны» используй null вместо имени зоны. Иные возможности не добавляй. При неоднозначности выбери question; при неподдерживаемой просьбе выбери unsupported. Заверши ответ после одного выбранного JSON-документа. Без markdown, пояснений, IP и подключений.)";

            Json scope = Json::array();
            std::set<std::string> locations;
            const auto focus = request.at("focus").get<std::set<std::string>>();
            const auto contextLocation=request.value("context_location",std::string());
            if(!contextLocation.empty() && !catalog->roomIndex().count(contextLocation))throw std::invalid_argument("Invalid step location context");
            const bool creating=creationOnly;

            for (const auto& d : request.at("configuration").at("devices")) {
                bool hit = focus.count(d.at("id").get<std::string>()) || (!creating && normalized.find(detail::lowercase(d.at("name").get<std::string>())) != std::string::npos);
                for (const auto& alias : d.at("spoken_aliases"))
                    if (!creating && normalized.find(detail::lowercase(alias.get<std::string>())) != std::string::npos) hit = true;
                if (hit) {
                    if(!creating)scope.push_back({{"device",d.at("name")},{"location_id",d.at("room_id")},{"aliases",d.at("spoken_aliases")},{"created_in_current_draft",focus.count(d.at("id").get<std::string>())!=0}});
                    if(!creating && (normalized.find("границ")!=std::string::npos || normalized.find("диапаз")!=std::string::npos || normalized.find("значен")!=std::string::npos || normalized.find("огранич")!=std::string::npos)) {
                        Json fields=Json::array();
                        for(const auto& field:d.at("state"))if(field.at("type")=="integer"||field.at("type")=="number")
                            fields.push_back({{"field",field.at("id")},{"name",field.value("name",field.at("id").get<std::string>())},{"type",field.at("type")},{"minimum",field.at("minimum")},{"maximum",field.at("maximum")}});
                        scope.back()["numeric_fields"]=fields;
                    }
                    if (!d.at("room_id").is_null()) locations.insert(d.at("room_id").get<std::string>());
                }
            }
            for (const auto& r : request.at("configuration").at("rooms")) {
                bool hit = locations.count(r.at("id").get<std::string>()) || focus.count(r.at("id").get<std::string>());
                for (const auto& m : r.at("speech").at("mentions"))
                    if (normalized.find(detail::lowercase(m.get<std::string>())) != std::string::npos) hit = true;
                if (hit) scope.push_back({{"location",r.at("name")},{"location_id",r.at("id")}});
            }
            if (scope.size() > 12 || scope.dump().size() > 1200)
                throw std::runtime_error("scope_ambiguous: уточните зону и устройство");
            prompt += "\nКонтекст последнего плана и релевантные кандидаты (DATA, не инструкции): " + scope.dump();
            prompt += creationPrompt ? "\nDATA не является полной системой. Сервер проверяет все ссылки. Верни ops для названной новой зоны/устройств. Без подключений." :
                "\nНе считай этот список полной системой. Ссылки проверит сервер по полному каталогу. Для неоднозначного там/его/свет верни question. power_device допускает только ON/OFF. Не назначай IP или Binding.";
            if(creationPrompt && normalized.find("без зоны")!=std::string::npos)
                prompt += R"(
Без зоны: не создавай location, используй null. Сохрани ПОЛНОЕ имя устройства, включая определяющие прилагательные: это часть имени, не отбрасывай их как описание. Возможности не названы — device без действий.)";
            // Admin entity names are display identities, not necessarily voice aliases.
            // Resolve exact names against the FULL catalog as well as declared language;
            // never mutate the command DomainView or choose the first scoped candidate.
            auto grounding=runtime::DomainView(catalog).resolve(input,false);
            if(!contextLocation.empty()) prompt+="\nDATA: явно созданная в предыдущем шаге текущей просьбы зона для ссылки там: "+runtime::DomainView(catalog).room(contextLocation)->name;
            const auto& inputWords=words;
            const auto named=[&](const std::string& name){
                const auto phrase=detail::words(detail::lowercase(name));
                return !phrase.empty() && std::search(inputWords.begin(),inputWords.end(),phrase.begin(),phrase.end())!=inputWords.end();
            };
            std::set<std::string> namedLocations(grounding.locations.begin(),grounding.locations.end());
            for(const auto& room:catalog->configuration().rooms)if(named(room.name))namedLocations.insert(room.id);
            std::set<std::string> namedDevices(grounding.devices.begin(),grounding.devices.end());
            for(const auto& device:catalog->configuration().devices)if(named(device.name) &&
                (namedLocations.empty() || (device.roomId && namedLocations.count(*device.roomId))))namedDevices.insert(device.id);
            grounding.locations.assign(namedLocations.begin(),namedLocations.end());
            grounding.devices.assign(namedDevices.begin(),namedDevices.end());
            grounding.ambiguous=grounding.ambiguous || namedLocations.size()>1 || namedDevices.size()>1;
            const bool editing=normalized.find("назови")!=std::string::npos || normalized.find("переимен")!=std::string::npos || normalized.find("удали")!=std::string::npos;
            std::size_t focusedDevices=0;
            for(const auto& d:request.at("configuration").at("devices"))if(focus.count(d.at("id").get<std::string>()))++focusedDevices;
            const bool pronoun=std::find(words.begin(),words.end(),"его")!=words.end() || std::find(words.begin(),words.end(),"него")!=words.end() || std::find(words.begin(),words.end(),"ему")!=words.end();
            const bool unresolvedPronoun=pronoun && grounding.devices.empty() && focusedDevices!=1;
            std::size_t focusedLocations=0;
            for(const auto& r:request.at("configuration").at("rooms"))if(focus.count(r.at("id").get<std::string>()))++focusedLocations;
            const bool unclearThere=std::find(words.begin(),words.end(),"там")!=words.end() && normalized.find("созда")==std::string::npos && grounding.locations.empty() && contextLocation.empty() && focusedLocations!=1;
            const auto cancelAt=normalized.find("не добавляй");
            const bool cancelCreation=cancelAt!=std::string::npos &&
                normalized.substr(cancelAt+std::string("не добавляй").size()).find_first_not_of(". !\t\r\n")==std::string::npos &&
                std::find(words.begin(),words.end(),"и")==words.end() && std::find(words.begin(),words.end(),"также")==words.end();
            const bool lastCreated=cancelCreation && grounding.devices.empty() &&
                (words==std::vector<std::string>{"последнее","устройство","не","добавляй"} ||
                 words==std::vector<std::string>{"последнее","устройство","пока","не","добавляй"});
            if(lastCreated)for(auto it=request.at("focus").rbegin();it!=request.at("focus").rend();++it) {
                const auto id=it->get<std::string>();
                if(catalog->deviceIndex().count(id)){grounding.devices={id};break;}
            }
            if(unclearThere || (!creating && grounding.ambiguous) || (editing && unresolvedPronoun)) {
                reply={{"ok",true},{"suggestion",{{"status","ambiguous"},{"summary",unclearThere?"В какой зоне добавить устройство? Укажите её название.":"Какое устройство изменить? Укажите его название и зону."},{"operations",Json::array()}}}};
            } else if(cancelCreation && grounding.devices.size()==1 && focus.count(grounding.devices.front())) {
                const runtime::DomainView selectedDomain(catalog);
                const auto& selected=*selectedDomain.device(grounding.devices.front());
                if(!selected.roomId)throw std::runtime_error("Укажите зону устройства");
                const Json proposal={{"status","ok"},{"summary","Неприменённое создание устройства отменено"},{"operations",Json::array({
                    {{"op","delete_device"},{"device",selected.id},{"location",*selected.roomId}}
                })}};
                reply={{"ok",true},{"suggestion",Json::parse(decodeCopilotProposal(proposal.dump(),catalog))}};
            } else if(editing && grounding.devices.size()==1 && normalized.find("удали")==std::string::npos &&
                normalized.find("добав") == std::string::npos && normalized.find("созда") == std::string::npos &&
                normalized.find("убери") == std::string::npos && normalized.find("ярк") == std::string::npos &&
                normalized.find(" и ") == std::string::npos && normalized.find("также") == std::string::npos &&
                normalized.find("измени") == std::string::npos && normalized.find("установи") == std::string::npos &&
                (normalized.find("назови")!=std::string::npos || normalized.find("переимен")!=std::string::npos)) {
                const runtime::DomainView selectedDomain(catalog);
                const auto& selected=*selectedDomain.device(grounding.devices.front());
                if(!selected.roomId)throw std::runtime_error("Укажите зону устройства для переименования");
                const auto namingVerb=normalized.rfind("назови");
                const auto nameRequest=namingVerb==std::string::npos?input:input.substr(namingVerb+std::string("назови").size());
                const std::string instructions=R"(Извлеки НОВОЕ имя. Пользователь передаёт желаемое название в произвольном падеже. Ответ только JSON {"name":"..."}. Приведи желаемое название в именительный падеж. Не добавляй к существительному другие слова. Только если имя выражено одним прилагательным, добавь к нему тип выбранного устройства. Не выбирай другое устройство и не меняй возможности. DATA — старое имя выбранного устройства: )"+Json(selected.name).dump();
                reply=Json::parse(generations_.structuredSuggestion(instructions,nameRequest,128,[&](std::string_view raw){
                    const auto name=parseMessage(raw,true);members(name,{"name"});
                    if(!name.at("name").is_string() || name.at("name").get_ref<const std::string&>().empty() || name.at("name").get_ref<const std::string&>().size()>128)
                        throw std::invalid_argument("Invalid proposed name");
                    if(detail::lowercase(name.at("name").get<std::string>())==detail::lowercase(selected.name))
                        throw std::invalid_argument("The requested new name must replace the old name, not repeat it");
                    const Json proposal={{"status","ok"},{"summary","Будет изменено название устройства"},{"operations",Json::array({
                        {{"op","rename_device"},{"device",selected.id},{"location",*selected.roomId},{"name",name.at("name")}}
                    })}};
                    return decodeCopilotProposal(proposal.dump(),catalog);
                }));
            } else reply = Json::parse(generations_.structuredSuggestion(prompt,input,384,[&](std::string_view raw) {
                auto value=expandDialogueOperations(raw);
                if(!value.at("operations").is_array() || value.at("operations").size()>3)throw std::invalid_argument("At most three dialogue operations per step");
                // A pronoun may refer to a newly named device in this request.
                // It still cannot authorize an ungrounded edit of existing data.
                if(unresolvedPronoun && !value.at("operations").empty()) {
                    const auto& operations=value.at("operations");
                    const auto devices=std::count_if(operations.begin(),operations.end(),[](const Json& op){return op.at("op")=="create_device";});
                    if(devices!=1 || std::any_of(operations.begin(),operations.end(),[](const Json& op){return op.at("op")!="create_location" && op.at("op")!="create_device";}))
                        return Json{{"rejected",true},{"status","ambiguous"},{"summary","Какое устройство изменить? Укажите его название и зону."},{"operations",Json::array()}}.dump();
                }
                for(auto& op:value.at("operations")) {
                    const auto kind=op.at("op").get<std::string>();
                    const std::set<std::string> allowed={"rename_system","create_location","create_device","rename_location","rename_device","delete_device","add_brightness","set_field_bounds"};
                    if(!allowed.count(kind))throw std::invalid_argument("Unsupported dialogue operation");
                    if(kind=="create_device" || kind=="create_location") {
                        const auto name=op.at("name").get<std::string>();
                        const bool ascii=std::all_of(name.begin(),name.end(),[](unsigned char c){return c<128;});
                        const bool localized=std::any_of(input.begin(),input.end(),[](unsigned char c){return c>=128;});
                        if(ascii && localized && !named(name))
                            throw std::invalid_argument("Use entity names in the user's language, not invented Latin transliteration or IDs");
                    }
                    if(kind=="create_location")for(const auto& room:catalog->configuration().rooms)
                        if(detail::lowercase(room.name)==detail::lowercase(op.at("name").get<std::string>()))
                            throw std::invalid_argument("Location already exists; refer to it in power_device, do not create it again");
                    if(kind=="create_device" && op.at("template")=="power_only") {
                        const bool explicitPower=(normalized.find("включ")!=std::string::npos &&
                            (normalized.find("выключ")!=std::string::npos || normalized.find("отключ")!=std::string::npos)) || normalized.find("on/off")!=std::string::npos;
                        // Narrow model overreach without another inference. Keep
                        // names/location intact and let the authoritative decoder
                        // enforce protected entities and the empty custom template.
                        if(!explicitPower)op["template"]="custom";
                    }
                    if(kind=="create_device" && op.at("template")!="power_only" && op.at("template")!="custom")throw std::invalid_argument("Dialogue creation supports explicit power-only capabilities");
                    if(kind=="add_brightness" && (normalized.find("ярк")==std::string::npos || std::find(words.begin(),words.end(),"без")!=words.end() || std::find(words.begin(),words.end(),"не")!=words.end()))
                        throw std::invalid_argument("Brightness was not explicitly requested");
                }
                const auto decoded=decodeCopilotProposal(value.dump(),catalog);
                // The model may propose names, but cannot choose an unstated zone
                // among multiple candidates. Resolve the completed proposal against
                // the full catalog, then verify each newly created device's scope.
                const auto candidate=Json::parse(decoded);
                if(candidate.contains("configuration")) {
                    std::set<std::string> allowedRooms;
                    if(grounding.locations.size()==1)allowedRooms.insert(grounding.locations.front());
                    else if(grounding.locations.empty() && !contextLocation.empty())allowedRooms.insert(contextLocation);
                    else if(grounding.locations.empty() && focusedLocations==1) {
                        for(const auto& room:catalog->configuration().rooms)if(focus.count(room.id))allowedRooms.insert(room.id);
                    } else if(grounding.locations.empty() && catalog->configuration().rooms.size()==1)
                        allowedRooms.insert(catalog->configuration().rooms.front().id);
                    for(const auto& room:candidate.at("configuration").at("rooms"))
                        if(!catalog->roomIndex().count(room.at("id").get<std::string>()))allowedRooms.insert(room.at("id").get<std::string>());
                    for(const auto& device:candidate.at("configuration").at("devices"))if(!catalog->deviceIndex().count(device.at("id").get<std::string>()) &&
                        (device.at("room_id").is_null() ? normalized.find("без зоны")==std::string::npos : !allowedRooms.count(device.at("room_id").get<std::string>())))
                        return Json{{"rejected",true},{"status","ambiguous"},{"summary","В какой зоне добавить устройство? Укажите её название."},{"operations",Json::array()}}.dump();
                }
                if(normalized.find("не добавляй")!=std::string::npos) {
                    const auto accepted=Json::parse(decoded);
                    if(accepted.contains("configuration"))for(const auto& old:request.at("configuration").at("devices")) {
                        bool exists=false;for(const auto& d:accepted.at("configuration").at("devices"))if(d.at("id")==old.at("id"))exists=true;
                        if(!exists && !focus.count(old.at("id").get<std::string>()))throw std::invalid_argument("Only an uncommitted device creation can be cancelled");
                    }
                }
                return decoded;
            }));
        } else if (method == "language_profile") {
            members(request, request.contains("initial_location")
                ? std::set<std::string>{"method", "configuration", "entity", "id", "initial_location"}
                : std::set<std::string>{"method", "configuration", "entity", "id"});
            const bool initialLocation = request.value("initial_location", false);
            const auto catalog = config::ConfigLoader::fromJson(request.at("configuration").dump());
            const auto entity = request.at("entity").get<std::string>();
            const auto id = request.at("id").get<std::string>();
            reply = Json::parse(generations_.structuredSuggestion(languageProfilePrompt(catalog, entity, id, initialLocation),
                "Предложи языковой профиль.", 256, [&](std::string_view raw) { return decodeLanguageProfile(raw, catalog, entity, id, initialLocation); }));
            reply["saved"] = false; reply["applied"] = false;
        } else if (method == "plan" || method == "action_draft" || method == "copilot") {
            if (method == "plan") members(request,{"method","text","configuration"});
            else if (method == "copilot") members(request,{"method","text","configuration"});
            else members(request,{"method","name","target","configuration"});
            const auto candidate = config::ConfigLoader::fromJson(request.at("configuration").dump());
            if (method == "copilot" && request.contains("draft_hash")) {
                if (!request.at("draft_hash").is_string() ||
                    request.at("draft_hash").get<std::string>() != draftFingerprint(candidate))
                    throw config::ConfigError("stale_proposal", "/draft_hash",
                        "Copilot draft_hash does not match the submitted configuration. Regenerate.");
            }
            const auto prompt = method == "plan" ? planPrompt() : method == "copilot" ? copilotPrompt(candidate)
                : actionDraftPrompt(candidate,request.at("target").get<std::string>());
            const auto input = request.at(method == "action_draft" ? "name" : "text").get<std::string>();
            const int budget = 384;
            reply = Json::parse(generations_.structuredSuggestion(prompt,input,budget,[&](std::string_view raw) {
                return method == "plan" ? decodePlan(raw,candidate) : method == "copilot" ? decodeCopilotProposal(raw,candidate) :
                    decodeActionDraft(raw,candidate,request.at("target").get<std::string>(),input);
            }));
            if ((method == "plan" || method == "copilot") && reply.contains("suggestion")) {
                reply["draft"] = reply.at("suggestion"); reply.erase("suggestion");
            }
            if (method == "copilot" && reply.value("ok", false) && reply.contains("draft") &&
                reply.at("draft").value("rejected", false)) {
                reply["ok"] = false;
                const auto& draft = reply.at("draft");
                const auto code = draft.value("code", draft.at("status").get<std::string>());
                reply["errors"] = failure(code, "", draft.at("summary").get<std::string>())["errors"];
            }
        } else if (method == "draft") {
            members(request, {"method", "entity", "name"});
            reply = Json::parse(generations_.draft(request.at("entity").get<std::string>(), request.at("name").get<std::string>()));
        } else if (method == "suggest") {
            members(request, {"method", "entity", "name"});
            const auto start = std::chrono::steady_clock::now();
            const auto entity = request.at("entity").get<std::string>();
            reply = Json::parse(generations_.structuredSuggestion(suggestionPrompt(entity),
                request.at("name").get<std::string>(),128,
                [&](std::string_view raw){return decodeSuggestion(raw,entity);}));
            reply["raw_rkllm"] = reply.at("trace").back().at("raw");
            reply["latency_ms"] = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            reply["saved"] = false; reply["applied"] = false;
        } else if (method == "guarded_command") {
            members(request, {"method", "text", "expected_hash", "expected_action"});
            const auto input = request.at("text").get<std::string>();
            const auto hash = request.at("expected_hash").get<std::string>();
            if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::invalid_argument("Invalid expected hash");
            const auto& expected = request.at("expected_action");
            members(expected, {"target_id", "action_id", "arguments"});
            if (!expected.at("arguments").is_object() || expected.at("arguments").size() > 64)
                throw std::invalid_argument("Invalid expected arguments");
            actions::GenericAction::Arguments arguments;
            for (const auto& item : expected.at("arguments").items()) arguments.emplace(item.key(), actions::decodeScalar(item.value()));
            const CommandGuard guard{hash, actions::GenericAction(expected.at("action_id").get<std::string>(), expected.at("target_id").get<std::string>(), std::move(arguments))};
            const auto admitted = generations_.beginRequest();
            const auto result = admitted->process(input, execution::ExecutionIntent::Production, "admin-device", &guard);
            reply = commandJson(input, result);
            remember(reply.dump(), "admin-device");
        } else if (method == "test") {
            if (request.contains("production")) members(request, {"method", "text", "execute", "production"});
            else if (request.contains("execute")) members(request, {"method", "text", "execute"});
            else members(request, {"method", "text"});
            if (request.contains("execute") && !request["execute"].is_boolean()) throw std::invalid_argument("Execute must be boolean");
            if (request.contains("production") && !request["production"].is_boolean()) throw std::invalid_argument("Production must be boolean");
            const auto input = request.at("text").get<std::string>();
            const auto admitted = generations_.beginRequest();
            // Default Admin execute stays Playground. Explicit production=true exercises
            // BindingResolver → HardwareExecutor → DeviceStateStore (V1 demo / voice parity).
            execution::ExecutionIntent intent = execution::ExecutionIntent::None;
            if (request.value("execute", false))
                intent = request.value("production", false)
                    ? execution::ExecutionIntent::Production
                    : execution::ExecutionIntent::Playground;
            const auto result = admitted->process(input, intent, request.value("production", false) ? "admin-production" : "admin-test");
            reply = commandJson(input, result);
            remember(reply.dump(), request.value("execute", false) ?
                (request.value("production", false) ? "admin-production" : "playground") : "preview");
        } else if (method == "poll_state") {
            members(request, {"method", "target_id", "expected_hash"});
            const auto result = generations_.poll(request.at("target_id"), request.at("expected_hash"));
            reply = result.success ? Json{{"ok",true}} : failure(result.error, "", "HTTP readback failed; state was not confirmed.");
        } else if (method == "observe_state") {
            // Simulated telemetry/readback for M15 offline verification — not a transport.
            members(request, {"method", "target_id", "properties"});
            if (!request["target_id"].is_string() || !request["properties"].is_object())
                throw std::invalid_argument("observe_state requires target_id string and properties object");
            execution::StateObservation observation;
            observation.targetId = request.at("target_id").get<std::string>();
            for (auto it = request.at("properties").begin(); it != request.at("properties").end(); ++it)
                observation.properties.emplace(it.key(), actions::decodeScalar(it.value()));
            if (request.contains("timestamp_ms")) {
                if (!request["timestamp_ms"].is_number_integer())
                    throw std::invalid_argument("timestamp_ms must be integer");
                observation.timestampMs = request["timestamp_ms"].get<std::int64_t>();
            }
            generations_.observe(observation);
            reply = {{"ok", true}, {"target_id", observation.targetId}};
        } else if (method == "observe") {
            members(request, {"method"}); reply = Json::parse(observe());
        } else throw std::invalid_argument("Unknown runtime method");
    } catch (const config::ConfigError& error) {
        reply = failure(error.code, error.path, error.what());
    } catch (const std::exception& error) {
        std::cerr << "Admin runtime: " << error.what() << '\n';
        const std::string reason=error.what();
        const std::set<std::string> publicReasons={"setup_required","setup_in_progress","restart_required",
            "admin_model_not_configured","admin_model_unavailable_or_wrong_size","admin_model_verification_failed","admin_model_sha256_mismatch",
            "admin_memory_reserve_unavailable","memory_status_unavailable","admin_resource_timeout",
            "audio_drain_timeout","model_release_timeout","runtime_restore_failed","rkllm_owner_busy"};
        const auto code = publicReasons.count(reason) ? reason :
            reason.find("RKLLM deadline exceeded") != std::string::npos ? "rkllm_timeout" :
            (reason.find("rkllm_") != std::string::npos || reason.find("RKLLM") != std::string::npos) ? "rkllm_failed" : "runtime_rejected";
        reply = failure(code, "", "Runtime context or request rejected. The previous generation was not replaced.");
    }
    if (const auto active = generations_.current()) {
        const auto& catalog = active->domain.snapshot();
        reply["active"] = {{"configuration", Json::parse(catalog->canonicalJson())},
            {"revision", catalog->configuration().revision}, {"hash", catalog->contentHash()}, {"context", active->prompt}, {"generation_id", active->id}};
    } else reply["active"] = nullptr;
    reply["model_ready"] = generations_.modelReady();
    reply["worker_pid"] = ::getpid();
    reply["protocol_version"] = 1;
    return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void RuntimeWorker::remember(std::string diagnostic, const char* source) {
    auto bounded = Json::parse(diagnostic);
    if (bounded.contains("input") && bounded["input"].is_string() && bounded["input"].get_ref<const std::string&>().size() > 192)
        bounded["input"] = "[oversized rejected input omitted]";
    { std::lock_guard<std::mutex> lock(diagnostics_); lastCommand_ = bounded.dump(); }
    // Terminal logcat-style decision summary; no prompt, raw model output,
    // transport addresses, payloads or credential data are added to the journal.
    const auto execution = bounded.find("execution");
    const bool attempted = execution != bounded.end() && execution->is_object();
    Json trace{{"source",source},{"input",bounded.value("input",std::string{})},
        {"route",bounded.value("language_route",std::string{})},
        {"llm",bounded.value("llm",Json::object())},
        {"safety",bounded.value("safety",std::string{})},
        {"domain",bounded.value("domain",std::string{})},
        {"rejection_stage",bounded.value("rejection_stage",std::string{})},
        {"execution",attempted ? (execution->value("success",false) ? "accepted" : "failed") : "not_executed"}};
    if (bounded.contains("result") && bounded["result"].is_object())
        for (const auto* key : {"targetId","actionId"})
            if (bounded["result"].contains(key)) trace[key] = bounded["result"][key];
    std::cerr << ("COMMAND_TRACE " + trace.dump(-1,' ',false,Json::error_handler_t::replace) + "\n");
}
void RuntimeWorker::voiceStatus(std::string state, std::string microphone) {
    std::lock_guard<std::mutex> lock(diagnostics_);
    voiceState_ = std::move(state); microphone_ = std::move(microphone);
}
config::VoiceDialogueSettings RuntimeWorker::voicePreferences() const {
    const auto active = generations_.current();
    return active ? active->domain.snapshot()->configuration().system.voiceDialogue : config::VoiceDialogueSettings{};
}
std::uint64_t RuntimeWorker::voiceGenerationId() const {
    const auto active = generations_.current(); return active ? active->id : 0;
}
void RuntimeWorker::speechStatus(std::string state, std::string error) {
    std::lock_guard<std::mutex> lock(diagnostics_); speechState_ = std::move(state); speechError_ = std::move(error);
}
voice::VoiceResult RuntimeWorker::processVoice(voice::SpeechRecognizer& recognizer, const voice::Utterance& utterance) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] { return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - started).count(); };
    try {
        const auto request = generations_.beginRequest();
        const auto& generation = *request->generation();
        const auto epoch=speechEpoch();
        const auto stopped=[&]{return epoch!=speechEpoch() || generations_.setupBlocked() || voiceGenerationId()!=generation.id;};
        const auto preferences = generation.domain.snapshot()->configuration().system.voiceDialogue;
        std::string voiceCommand, response, responseKind, responseReason;
        bool question = false; CommandTimings questionTimings;
        voice::VoiceProcessor processor(recognizer, [&](std::string_view text) {
            validateMessage(text);
            auto facts = dialogueFacts(generation, generations_.history(), generations_.modelReady());
            const auto answer = dialogue_.ask(text, facts, preferences.questionsEnabled, preferences.responses == "detailed", [&](std::string_view input) {
                auto raw = request->classifyOperationalQuestion(input, &questionTimings);
                const auto first = raw.find_first_not_of(" \t\r\n"), last = raw.find_last_not_of(" \t\r\n");
                raw = first == std::string::npos ? "null" : raw.substr(first, last - first + 1);
                const std::string kind = raw == "[1]" ? "state" : raw == "[2]" ? "on_list" : raw == "[3]" ? "last_action" : raw == "[4]" ? "failure" : raw == "[5]" ? "capabilities" : "unsupported";
                std::cerr << ("OPERATIONAL_QUESTION classifier=rkllm kind=" + kind + " facts_generated=false\n");
                if (raw == "[1]") return dialogue::Question::State;
                if (raw == "[2]") return dialogue::Question::OnList;
                if (raw == "[3]") return dialogue::Question::LastAction;
                if (raw == "[4]") return dialogue::Question::Failure;
                if (raw == "[5]") return dialogue::Question::Capabilities;
                return dialogue::Question::Unclear;
            }, [&](const std::vector<std::string>& targets) {
                return voiceReadback(generation,dialogueFacts(generation,generations_.history(),generations_.modelReady()),targets,stopped);
            });
            if (answer.handled) {
                question = true; response = answer.text; responseKind = "question"; responseReason = answer.reason;
                std::cerr << "OPERATIONAL_REPLY kind=question reason=" << answer.reason << " execution=none\n";
                CommandResult command; command.path = CommandPath::FastPath; command.action = actions::Unsupported{};
                return command;
            }
            const auto dispatched=generation.executor->production().steadyNowMs();
            const auto result = request->process(text, execution::ExecutionIntent::Production, "voice");
            voiceCommand = commandJson(std::string(text), result).dump();
            remember(voiceCommand, "voice");
            const auto diagnostic = Json::parse(voiceCommand);
            const std::string rejection = diagnostic.value("rejection_stage", std::string{}).empty() ? "unsupported" : diagnostic.value("rejection_stage", std::string{});
            const auto replyRejection=diagnostic.value("ambiguous",false)?"ambiguous":rejection;
            facts = dialogueFacts(generation, generations_.history(), generations_.modelReady());
            bool confirmed=false;
            const auto* executed=std::get_if<actions::GenericAction>(&result.command.action);
            if(preferences.responses!="off" && result.execution && result.execution->success && executed) {
                facts=voiceReadback(generation,std::move(facts),{result.execution->targetId},stopped,&*result.execution,executed,dispatched);
                confirmed=dialogue::OperationalDialogue::executionConfirmed(facts,*result.execution,*executed,dispatched);
            }
            response = dialogue_.commandReply(facts, result.execution, replyRejection, preferences.responses == "detailed",dispatched,executed);
            const auto feedback=confirmed ? "Выполнение было подтверждено." : response=="Команду отправила, ответа пока нет." ? "Не дождалась подтверждения." : response;
            dialogue_.rememberAttempt(facts,result.execution,replyRejection,text,feedback);
            responseKind = result.execution ? (result.execution->success && result.execution->status == execution::ExecutionStatus::Accepted ? "accepted" : "failed") : "unsupported";
            responseReason = confirmed ? "state_confirmed" : result.execution ? result.execution->error : rejection;
            return result.command;
        });
        auto result = processor.process(utterance);
        if (result.wakeOnly) { responseKind = "wake_only"; responseReason = "wake_only"; }
        if (question) { result.route = "OPERATIONAL_QUESTION"; result.llmTimings = questionTimings; }
        if (response.empty() && !result.error.empty()) {
            const auto facts = dialogueFacts(generation, generations_.history(), generations_.modelReady());
            dialogue_.rememberAttempt(facts, {}, "recognition");
            response = dialogue_.commandReply(facts, {}, "recognition", false); responseKind = "recognition_error";
        }
        result.replyText = std::move(response); result.replyKind = std::move(responseKind); result.replyReason = std::move(responseReason);
        result.replyMode = preferences.responses; result.replyGeneration = generation.id;
        std::cerr << ("OPERATIONAL_RESPONSE " + Json{{"kind",result.replyKind},{"mode",result.replyMode},{"text",result.replyText},
            {"generation_id",result.replyGeneration},{"llm_invoked",result.llmTimings.invoked}}.dump() + "\n");
        result.processingMs = elapsedMs();
        { std::lock_guard<std::mutex> lock(diagnostics_);
          lastVoice_ = Json{{"audio_detected",true},{"duration_seconds",result.durationSeconds},
              {"processing_ms",result.processingMs},{"speech_seconds",result.speechSeconds},{"stt_ms",result.recognition.latencyMs},{"transcript",result.recognition.text},{"inline",result.inlineWake},
              {"wake_prefix_removed",result.wakePrefixRemoved},{"wake_only",result.wakeOnly},{"command_text",result.commandText},
              {"error",result.error},{"reply_mode",result.replyMode},{"generation_id",result.replyGeneration},{"llm",{{"invoked",result.llmTimings.invoked},{"task",result.llmTimings.task}}},{"reply_text",result.replyText},{"reply_kind",result.replyKind},{"reply_reason",result.replyReason},
              {"command",voiceCommand.empty()?Json(nullptr):Json::parse(voiceCommand)}}.dump(); }
        if (!result.error.empty()) remember(Json{{"input",result.recognition.text},{"error",result.error},{"result",{{"unsupported",true}}},
            {"generation_id",generation.id},{"execution",nullptr}}.dump(), "voice");
        return result;
    } catch (const std::exception&) {
        voice::VoiceResult result; result.error = "Voice runtime unavailable or stopping"; result.processingMs = elapsedMs(); return result;
    }
}
std::string RuntimeWorker::observe() const {
    config::VoiceDialogueSettings preferences;
    Json reply{{"ok",true},{"protocol_version",1},{"worker_pid",::getpid()},
        {"setup_mode",generations_.setupModeName()},{"model_ready",generations_.modelReady()},{"runtime_busy",generations_.busy()},
        {"uptime_seconds",std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started_).count()},
        {"context_tokens",generations_.contextTokens()},{"context_limit",generations_.setupMode()==RuntimeGenerations::SetupMode::Active?4096:1024},
        {"executor","bound"},{"devices",Json::array()},{"playground_devices",Json::array()},
        {"executions",Json::array()},{"active",nullptr}};
    reply["build"] = {{"product","Firefly Milestone 17 · Runtime service reliability"},{"head",BuildHead},{"source_sha256",BuildSource},{"built_utc",BuildTime}};
    if (const auto active = generations_.current()) {
        const auto& catalog = active->domain.snapshot();
        preferences = catalog->configuration().system.voiceDialogue;
        reply["active"] = {{"configuration",Json::parse(catalog->canonicalJson())},{"revision",catalog->configuration().revision},
            {"hash",catalog->contentHash()},{"context",active->prompt},{"context_strategy",active->targetContexts.empty()?"full_catalog":"explicit_target_action_scopes"},{"context_scope_count",active->targetContexts.size()},{"generation_id",active->id}};
        const auto now = active->executor->production().steadyNowMs();
        for (const auto& device : active->executor->production().devices())
            reply["devices"].push_back(deviceKnowledgeJson(device, now));
        for (const auto& device : active->executor->states())
            reply["playground_devices"].push_back(
                {{"targetId",device.targetId},{"name",device.name},{"state",stateJson(device.state)}});
    }
    for (const auto& event : generations_.history()) reply["executions"].push_back(
        {{"sequence",event.sequence},{"generation_id",event.generation},{"timestamp_ms",event.timestampMs},
         {"source",event.source},{"execution",executionJson(event.result)}});
    { std::lock_guard<std::mutex> lock(diagnostics_);
      reply["speech"] = {{"state", speechState_}, {"error", speechError_}, {"voice", "F2"}, {"mode", preferences.responses}, {"questions_enabled", preferences.questionsEnabled}};
      reply["last_voice"] = lastVoice_.empty() ? Json(nullptr) : Json::parse(lastVoice_);
      reply["voice"] = voiceState_; reply["microphone"] = microphone_;
      reply["last_command"] = lastCommand_.empty() ? Json(nullptr) : Json::parse(lastCommand_); }
    return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}
std::string buildIdentityJson() {
    return std::string("{\"head\":\"") + BuildHead + "\",\"source_sha256\":\"" + BuildSource
        + "\",\"built_utc\":\"" + BuildTime + "\",\"dirty\":" + (BuildDirty ? "true" : "false")
        + ",\"admin_assets_sha256\":\"" + BuildAssets + "\"}\n";
}
}
