#include "firefly/admin/RuntimeWorker.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/config/ConfigStore.h"
#include "firefly/actions/CompactV1Adapter.h"
#include "firefly/llm/StructuredOutput.h"
#include "firefly/llm/LlmContext.h"
#include "firefly/SafetyGate.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
#include <future>
#include <unistd.h>

using namespace firefly;
using Json = nlohmann::json;
int main(int argc, char** argv) {
    int checks = 0;
    const auto check = [&](bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); };
    const auto rejects = [&](auto fn) { ++checks; try { fn(); } catch (const std::exception&) { return; } throw std::runtime_error("Expected rejection"); };
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const auto old = config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/current-m4.json");
        auto migrated = config::ConfigValidator::validate(config::migrateV2(old->configuration()));
        check(migrated->contentHash() != old->contentHash(), "Migration changes semantic hash");
        check(config::ConfigLoader::fromJson(migrated->canonicalJson())->canonicalJson() == migrated->canonicalJson(), "v2 round trip");
        check(config::ConfigValidator::validate(config::migrateV2(migrated->configuration()))->canonicalJson() == migrated->canonicalJson(), "Migration idempotent");
        for (const auto& d : old->configuration().devices) check(migrated->deviceIndex().count(d.id), "Stable IDs survive migration");
        auto priorMetadata=Json::parse(old->canonicalJson());priorMetadata["rooms"][0]["metadata"]={{"type","workshop"},{"description","Описание пользователя"},{"aliases",{"мастерская"}}};
        const auto priorCatalog=config::ConfigLoader::fromJson(priorMetadata.dump());
        const auto metadataMigration=config::ConfigValidator::validate(config::migrateV2(priorCatalog->configuration()));
        check(Json::parse(metadataMigration->canonicalJson())["rooms"][0]["metadata"]==Json::parse(priorCatalog->canonicalJson())["rooms"][0]["metadata"],"v1 user metadata survives versioned migration unchanged");
        auto document = Json::parse(migrated->canonicalJson());
        document["rooms"].push_back({{"id","location_children"},{"name","Детская комната"},{"speech",{{"mentions",{"детская","детской","детскую","комната для детей","комнате для детей"}},{"locations",{"в детской","в детской комнате","в комнате для детей"}}}}});
        const auto templates = Json::parse(admin::templatesJson());
        auto tv = *std::find_if(templates.begin(), templates.end(), [](const auto& t) { return t.at("id") == "tv"; });
        Json ids = Json::array();
        for (auto a : tv.at("actions")) { a["id"] = "action_tv_" + a.at("id").get<std::string>(); ids.push_back(a.at("id")); document["actions"].push_back(a); }
        document["devices"].push_back({{"id","device_tv"},{"name","ТВ для детей"},{"kind","generic"},{"room_id","location_children"},{"enabled",true},{"action_ids",ids},
            {"spoken_aliases",{"телевизор","телевизоре","детский телевизор","детском телевизоре","телевизор для детей"}},{"state",tv.at("state")}});
        auto catalog = config::ConfigLoader::fromJson(document.dump());
        runtime::DomainView domain(catalog); execution::MockDeviceExecutor executor(catalog);
        const auto setterContext = llm::generateContext(domain, "device_tv", "action_tv_volume");
        check(setterContext.rules.find("Обязательные ключи: [\"volume\"]") != std::string::npos,
              "Scoped setter requires the configured argument instead of an empty exact response");
        check(setterContext.rules.find("Точная форма ответа") == std::string::npos,
              "Parameterized action does not advertise an incomplete action as its exact form");
        check(llm::generateContext(domain, "device_tv", "action_tv_on").rules.find("Точная форма ответа") != std::string::npos,
              "No-argument action retains its complete empty-arguments contract");
        rejects([&]{llm::generateContext(domain, "device_tv", "action_set_brightness");});
        const auto decode = [&](const char* operation, Json args = Json::object()) {
            return llm::StructuredDecoder(domain).decode(Json{{"actionId",std::string("action_tv_") + operation},{"targetId","device_tv"},{"arguments",args}}.dump());
        };
        const auto candidate = [&](const char* operation, Json args = Json::object()) { return std::get<actions::GenericAction>(decode(operation,args).value); };
        auto on = candidate("on");
        check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply("Включи телевизор в детской", on, domain)), "TV grounding by location and noun");
        check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply("Включи детский телевизор", on, domain)), "TV full alias grounding");
        for (const auto* text : {"Не включи телевизор в детской","Включи и выключи телевизор в детской","Включи телевизор в спальне","Включи в детской"})
            check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply(text,on,domain)), "Direct unsafe candidate rejected");
        const auto before = executor.states();
        check(executor.preview(on).success, "Preview valid");
        check(std::get<execution::TypedState>(executor.states().back().state).at("power") == std::get<execution::TypedState>(before.back().state).at("power"), "Preview unchanged");
        check(executor.execute(on).success, "TV on execute");
        const auto onAgain = executor.execute(on); check(onAgain.previous == onAgain.current, "Setter idempotent");
        check(executor.execute(candidate("off")).success, "TV off execute");
        check(executor.execute(candidate("volume",{{"volume",35}})).success, "Volume setter");
        check(executor.execute(candidate("channel",{{"channel",7}})).success, "Channel setter");
        for (const auto& args : {Json{{"volume","40"}},Json{{"volume",true}},Json{{"volume",40.5}},Json{{"volume",101}},Json{{"volume",40},{"extra",1}},Json::object()})
            check(!decode("volume",args).valid(), "Strict invalid args");
        check(!decode("channel",{{"channel",0}}).valid(), "Channel minimum");
        check(!decode("channel",{{"channel",1000}}).valid(), "Channel maximum");
        const auto invalid = actions::GenericAction("action_tv_volume","device_tv",{{"volume",actions::Scalar(std::int64_t{101})}});
        const auto rejected = executor.execute(invalid); check(!rejected.success && !rejected.current, "Invalid execution has no mutation");
        check(std::holds_alternative<actions::CompactV1Error>(actions::CompactV1Adapter(catalog).encode(on)), "Generic action need not have compact encoding");
        auto oldLight = llm::StructuredDecoder(domain).decode(R"({"actionId":"action_set_brightness","targetId":"device_living_light","arguments":{"brightness":55}})").value;
        for (const auto* text : {"В гостинной включи свет на 55 процентов","В гостинной включии свет на 55 проццентов"}) {
            const auto evidence = domain.resolve(text);
            check(evidence.locations == std::vector<std::string>{"room_living"}, "Typo entity resolution");
            check(std::any_of(evidence.mentions.begin(),evidence.mentions.end(),[](const auto& m){return m.distance==1;}), "Auditable typo evidence");
            const bool exactOperation = std::string(text).find("включии") == std::string::npos;
            check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply(text,oldLight,domain)) == exactOperation,
                  "Entity typo remains grounded; unreviewed intent/unit typo cannot be ignored");
        }
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("В гостиной и спальне включи свет на 55 процентов",oldLight,domain)), "Multiple locations fail closed");
        auto contradiction = actions::GenericAction("action_set_brightness","device_kitchen_light",{{"brightness",actions::IntegerArgument(std::int64_t{0})}});
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("На кухне выключи свет на 50 процентов",contradiction,domain)), "contradiction02 direct guessed zero");
        // A real scoped-model probe invented 100 for "сделай свет ярче".
        // Check every candidate value directly, for both compatibility versions.
        for (const auto& safetyCatalog : {old, catalog}) {
            runtime::DomainView safetyDomain(safetyCatalog);
            for (const auto* text : {"На кухне сделай свет ярче", "На кухне сделай комфортный свет",
                     "На кухне немного света", "На кухне свет", "На кухне свет 101 процент",
                     "На кухне свет минус 10 процентов", "На кухне свет -1"}) {
                for (std::int64_t value = 0; value <= 100; ++value) {
                    const actions::GenericAction proposed("action_set_brightness", "device_kitchen_light", {{"brightness",actions::Scalar(value)}});
                    check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply(text,proposed,safetyDomain)), "Unspecified/invalid brightness rejects every injected value");
                }
            }
            for (const auto& [text, expected] : std::vector<std::pair<std::string,std::int64_t>>{
                     {"На кухне свет 40",40},{"На кухне поставь свет на сорок процентов",40},
                     {"На кухне включи свет",100},{"На кухне выключи свет",0}}) {
                for (std::int64_t value : {std::int64_t{0},std::int64_t{40},std::int64_t{100}}) {
                    const actions::GenericAction proposed("action_set_brightness", "device_kitchen_light", {{"brightness",actions::Scalar(value)}});
                    check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply(text,proposed,safetyDomain)) == (value==expected), "Light value must agree with original requirement");
                }
            }
        }
        auto door = actions::GenericAction("action_open_entrance","device_entrance_door",{});
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Открой входную дверь и включи телевизор в детской",door,domain)),"Door candidate cannot silently select one of two operations");
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Открой входную дверь и включи телевизор в детской",on,domain)),"Generic candidate cannot silently select one of two operations");
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Включи телевизор в детской и неизвестный прибор",on,domain)),"Unknown coordinated target is not ignored");
        auto compoundName=document;compoundName["devices"].back()["spoken_aliases"].push_back("панель и экран");
        runtime::DomainView compoundDomain(config::ConfigLoader::fromJson(compoundName.dump()));
        check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply("Включи панель и экран",on,compoundDomain)),"Declared compound entity name remains one fully grounded operation");
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Открой входную дверь и включи свет на кухне",door,domain)),"Protected door cannot ignore a light operation");
        const actions::GenericAction kitchenOn("action_set_brightness","device_kitchen_light",{{"brightness",actions::Scalar(std::int64_t{100})}});
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Открой входную дверь и включи свет на кухне",kitchenOn,domain)),"Light candidate cannot ignore a protected door operation");
        for (const auto* text : {"Открои входную дверь","Открой дверь","Закрой входную дверь","Не открой входную дверь"})
            check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply(text,door,domain)), "Door intent never corrected");
        auto duplicate = document; auto second = duplicate["devices"].back();second["id"]="device_tv_second";second["spoken_aliases"]={"телевизор","телевизоре"};duplicate["devices"].push_back(second);
        runtime::DomainView ambiguous(config::ConfigLoader::fromJson(duplicate.dump()));
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Включи телевизор в детской",on,ambiguous)), "Two TVs reject");
        check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply("Включи детский телевизор",on,ambiguous)), "Specific alias beats generic alias");
        auto renamed = document; renamed["devices"].back()["name"]="Новое имя";
        check(config::ConfigLoader::fromJson(renamed.dump())->deviceIndex().count("device_tv"), "Rename retains stable ID");
        auto missingTarget=document;missingTarget["devices"].back()["spoken_aliases"]={"включи"};rejects([&]{config::ConfigLoader::fromJson(missingTarget.dump());});
        auto blankIntent=document;blankIntent["actions"].back()["speech_aliases"]={" "};rejects([&]{config::ConfigLoader::fromJson(blankIntent.dump());});
        auto bad = document;bad["devices"][0]["action_ids"].push_back("action_tv_on");rejects([&]{config::ConfigLoader::fromJson(bad.dump());});
        bad=document;bad["actions"].back()["assignments"][0]["field"]="missing";rejects([&]{config::ConfigLoader::fromJson(bad.dump());});
        bad=document;bad["devices"].back()["state"][0]["initial"]="false";rejects([&]{config::ConfigLoader::fromJson(bad.dump());});
        auto tighter=document;tighter["devices"].back()["state"][1]["maximum"]=20;
        execution::MockDeviceExecutor incompatible(config::ConfigLoader::fromJson(tighter.dump()));rejects([&]{incompatible.reconcile(executor.states());});
        execution::MockDeviceExecutor retained(catalog);retained.reconcile(executor.states());check(retained.preview(on).previous==executor.preview(on).previous,"Apply retains compatible state");
        // Independent scalar schemas cover arbitrary operations, not template-specific code.
        auto typed=document;Json fields=Json::array(),parameters=Json::array(),assignments=Json::array();
        for(auto p : Json::array({
            Json{{"id","flag"},{"name","Флаг"},{"type","boolean"}},
            Json{{"id","count"},{"name","Счётчик"},{"type","integer"},{"minimum",-10},{"maximum",10}},
            Json{{"id","ratio"},{"name","Коэффициент"},{"type","number"},{"minimum",-2.5},{"maximum",2.5}},
            Json{{"id","label"},{"name","Метка"},{"type","string"},{"min_length",1},{"max_length",12}},
            Json{{"id","mode"},{"name","Режим"},{"type","enum"},{"values",{"forward","reverse"}}}})) {
            p["required"]=true;p["unit"]="";p["description"]="";parameters.push_back(p);
            const auto type=p["type"].get<std::string>();p["initial"]=type=="boolean"?Json(false):type=="string"?Json("start"):type=="enum"?Json("forward"):Json(0);fields.push_back(p);
            assignments.push_back({{"field",p["id"]},{"argument",p["id"]}});
        }
        typed["actions"].push_back({{"id","action_custom"},{"name","Обновить образец"},{"description","Присвоить значения виртуальному образцу"},{"operation","mock.assign.v1"},{"enabled",true},{"parameters",parameters},{"speech_aliases",{"обнови образец"}},{"assignments",assignments}});
        typed["devices"].push_back({{"id","device_sample"},{"name","Образец"},{"kind","generic"},{"room_id",nullptr},{"enabled",true},{"action_ids",{"action_custom"}},{"spoken_aliases",{"образец"}},{"state",fields}});
        const auto customCatalog=config::ConfigLoader::fromJson(typed.dump());runtime::DomainView customDomain(customCatalog);execution::MockDeviceExecutor customExecutor(customCatalog);
        const auto customContext = llm::generateContext(customDomain, "device_sample", "action_custom");
        for (const auto* key : {"flag", "count", "ratio", "label", "mode"})
            check(customContext.rules.find(std::string("\"") + key + "\"") != std::string::npos,
                  "Scoped custom contract lists required keys for every scalar kind");
        auto renamedTyped = typed;
        renamedTyped["devices"].back()["name"] = "Образец с меткой";
        check(llm::generateContext(runtime::DomainView(config::ConfigLoader::fromJson(renamedTyped.dump())),
              "device_sample", "action_custom").rules == customContext.rules,
              "Rename preserves required argument instructions and stable action identity");
        const auto customDecode=[&](const Json& args){return llm::StructuredDecoder(customDomain).decode(Json{{"actionId","action_custom"},{"targetId","device_sample"},{"arguments",args}}.dump());};
        Json valid{{"flag",true},{"count",5},{"ratio",0.75},{"label","готово"},{"mode","reverse"}};
        auto decoded=customDecode(valid);check(decoded.valid(),"Every scalar type decodes");check(customExecutor.execute(std::get<actions::GenericAction>(decoded.value)).success,"Atomic custom execution");
        for(const auto& entry : std::vector<std::pair<std::string,Json>>{{"flag",1},{"count",1.5},{"count",9007199254740992ULL},{"ratio","1.5"},{"label",false},{"label","слишком длинная метка"},{"mode","unknown"}}){auto args=valid;args[entry.first]=entry.second;check(!customDecode(args).valid(),"Bad scalar rejects without coercion");}
        auto emptyText=typed;for(auto& a:emptyText["actions"])if(a["id"]=="action_custom"){a["parameters"]=Json::array({a["parameters"][3]});a["parameters"][0]["min_length"]=0;a["assignments"]=Json::array({{{"field","label"},{"argument","label"}}});}
        for(auto& f:emptyText["devices"].back()["state"])if(f["id"]=="label")f["min_length"]=0;
        runtime::DomainView emptyDomain(config::ConfigLoader::fromJson(emptyText.dump()));
        const actions::GenericAction clearLabel("action_custom","device_sample",{{"label",actions::Scalar(std::string{})}});
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Обнови образец",clearLabel,emptyDomain)),"Empty string cannot be invented from missing input");
        check(std::holds_alternative<actions::GenericAction>(SafetyGate::apply("Обнови образец \"\"",clearLabel,emptyDomain)),"Explicit quoted empty string is grounded");
        auto atomic=typed;atomic["devices"].back()["state"][1]["maximum"]=2;runtime::DomainView atomicDomain(config::ConfigLoader::fromJson(atomic.dump()));execution::MockDeviceExecutor atomicExecutor(atomicDomain.snapshot());
        const auto failing=std::get<actions::GenericAction>(llm::StructuredDecoder(atomicDomain).decode(llm::structuredResult(decoded.value)).value);
        const auto atomicBefore=atomicExecutor.preview(failing);check(!atomicBefore.success,"State constraint failure rejects whole assignment set");
        const auto records=atomicExecutor.states();const auto rec=std::find_if(records.begin(),records.end(),[](const auto& r){return r.targetId=="device_sample";});
        check(std::get<execution::TypedState>(rec->state).at("flag")==actions::Scalar(false),"Earlier assignment not partially committed");
        rejects([&]{config::ConfigLoader::fromJson("{\"schema_version\":2,\"schema_version\":2}");});
        check(!llm::StructuredDecoder(customDomain).decode(R"({"actionId":"action_custom","actionId":"action_custom","targetId":"device_sample","arguments":{}})").valid(),"Duplicate action keys rejected");
        check(!llm::StructuredDecoder(customDomain).decode(R"({"actionId":"action_custom","targetId":"device_sample","arguments":{"ratio":1e400}})").valid(),"Nonfinite rejects");
        check(Json::parse(admin::decodeDraftGroup(R"({"type":"zone","description":"Зона для детей"})","location","identity")).contains("description"),"Full AI identity decode");
        check(Json::parse(admin::decodeDraftGroup(R"({"type":"ванная","description":"Место для водных процедур."})","location","identity")).at("type")=="ванная","Descriptive type follows the configuration UTF-8 contract, not an action profile enum");
        rejects([&]{admin::decodeDraftGroup(R"({"description":"","type":"zone"})","location","identity");});
        rejects([&]{admin::decodeDraftGroup(R"({"template":"shell","capabilities":[]})","device","capabilities");});
        const auto language=Json::parse(admin::decodeDraftGroup(R"({"name":"детская комната","name_location":"в детской комнате","short":"детская","short_location":"в детской","synonym":"комната для детей","synonym_location":"в комнате для детей"})","location","language"));
        check(language.at("mentions").size()==3 && language.at("locations").size()==3 &&
              language.at("aliases")==Json({"детская","детская комната","комната для детей"}),
              "One grammar draft keeps names separate from complete location phrases");
        rejects([&]{admin::decodeDraftGroup(R"({"name":"детская","name_location":"детской","short":"детская","short_location":"в детской","synonym":"детская","synonym_location":"в детской"})","location","language");});
        rejects([&]{admin::decodeDraftGroup(R"({"description":"Описание","type":"room","aliases":["детская","детская"]})","location","identity");});
        auto precise=valid;precise["ratio"]=1;
        const auto normalized=std::get<actions::GenericAction>(customDecode(precise).value);
        check(std::holds_alternative<double>(normalized.arguments().at("ratio").data()),"JSON number normalizes to its declared number type exactly");
        check(customExecutor.execute(normalized).success,"Declared number survives integer JSON wire representation");
        const auto reloaded=config::ConfigLoader::fromJson(customCatalog->canonicalJson());
        execution::MockDeviceExecutor numberReloaded(reloaded);numberReloaded.reconcile(customExecutor.states());
        check(numberReloaded.preview(normalized).success,"Number state survives serialize reload reconciliation");
        auto malformed=valid;malformed["ratio"]=0.12345678901234567;
        check(!customDecode(malformed).valid(),"Excess decimal precision rejected explicitly");
        config::Parameter textSchema;textSchema.type="string";textSchema.maxLength=20;
        check(!config::accepts(textSchema,actions::Scalar(std::string("\xc0\xaf"))),"Programmatic invalid UTF-8 rejected");
        auto legacyExtension=old->configuration();legacyExtension.actions.front().origin="local_ai";rejects([&]{config::ConfigValidator::validate(legacyExtension);});
        check(decode("volume",{{"volume",120}}).reason.find("0..100")!=std::string::npos,"Range rejection has a human reason");
        // Numbered namespaces and near-equal names never select the first item.
        auto numbered=document;
        for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"location_one","цех 1"},{"location_eleven","цех 11"},{"location_a","станция"},{"location_b","станциа"}})
            numbered["rooms"].push_back({{"id",pair.first},{"name",pair.second},{"speech",{{"mentions",{pair.second}},{"locations",{"в "+pair.second}}}}});
        auto collision=document;collision["rooms"].back()["metadata"]={{"aliases",{"кухне"}}};rejects([&]{config::ConfigLoader::fromJson(collision.dump());});
        runtime::DomainView numberedDomain(config::ConfigLoader::fromJson(numbered.dump()));
        check(numberedDomain.resolve("В цех 11 включи телевизор").locations==std::vector<std::string>{"location_eleven"},"Zone numbers exact");
        check(numberedDomain.resolve("В цех 111 включи телевизор").locations.empty(),"Zone number not corrected");
        check(numberedDomain.resolve("В станцие включи телевизор").ambiguous,"Equal fuzzy candidates reject");
        check(numberedDomain.resolve("В станция включи телевизор").locations==std::vector<std::string>{"location_a"},"Exact beats fuzzy");
        check(domain.resolve("Включи тб").devices.empty(),"Short aliases not corrected");
        const auto volume=candidate("volume",{{"volume",35}});
        for(const auto* text:{"На телевизоре в детской установи громкость 35.5 процентов","На телевизоре в детской установи громкость 35 40","Не установи громкость 35 на детском телевизоре"})
            check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply(text,volume,domain)),"Direct numeric/negative grounding rejects");
        check(std::holds_alternative<actions::Unsupported>(SafetyGate::apply("Выключи детский телевизор на 50 процентов",candidate("off"),domain)),"Setter cannot ignore an explicit value");
        check(!llm::StructuredDecoder(customDomain).decode(R"({"actionId":"action_custom","targetId":"device_sample","arguments":{"ratio":1e-400}})").valid(),"Underflow rejects");
        const auto parsedChannel=domain.parseDeclared("На детском телевизоре включи канал 7");
        check(parsedChannel==decode("channel",{{"channel",7}}).value,"Declared grammar selects channel before generic on phrase");
        struct Model final:admin::ContextModel {
            std::string context; bool available=true;std::string answer;
            void useContext(const std::string& value)override{if(!available)throw std::runtime_error("unavailable");context=value;}
            bool ready()const override{return available;}
            std::string infer(std::string_view)override{return answer;}
        } model;
        admin::RuntimeGenerations generations(model);generations.apply(catalog);
        auto lease=generations.beginRequest();
        auto apply=std::async(std::launch::async,[&]{generations.apply(config::ConfigLoader::fromJson(renamed.dump()));});
        check(apply.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"Apply waits for admitted generation");
        const auto completed=lease->process("Включи телевизор в детской",execution::ExecutionIntent::Playground);
        check(completed.generation->id==1&&completed.execution&&completed.execution->success,"Old lease completes with old generation/executor");
        lease.reset();apply.get();
        const auto next=generations.process("Выключи детский телевизор",execution::ExecutionIntent::Playground);
        check(next.generation->id==2&&next.execution&&next.execution->success,"Next request sees applied schema/aliases");
        check(std::get<execution::TypedState>(*next.execution->previous).at("power")==actions::Scalar(true),"State retained across generation");
        check(generations.current()->targetContexts.count("device_tv/action_tv_channel")&&generations.current()->requestPrompt("На детском телевизоре включи канал 7").find("action_tv_channel")!=std::string::npos,"Request context includes the complete explicitly requested binding");
        rejects([&]{generations.current()->requestPrompt("Включи неизвестное устройство");});
        model.available=false;rejects([&]{generations.draft("device","Устройство");});model.available=true;
        const auto plan=Json::parse(admin::decodePlan(R"({"location":{"name":"Участок упаковки","type":"workshop","description":"Место упаковки.","mentions":["упаковки"],"locations":["на участке упаковки"]},"devices":[{"name":"Освещение упаковки","template":"light","aliases":["свет упаковки"],"description":"Освещение участка."},{"name":"Вентилятор 1","template":"switch","aliases":["вентилятор 1"],"description":"Виртуальная вентиляция."},{"name":"Вентилятор 2","template":"switch","aliases":["вентилятор 2"],"description":"Виртуальная вентиляция."}]})",catalog));
        check(plan.at("configuration").at("devices").size()==catalog->configuration().devices.size()+3,"Bounded multi-entity graph validates");
        check(!plan.at("saved").get<bool>()&&!plan.at("applied").get<bool>(),"Plan never saves/applies");
        const auto officePlan=Json::parse(admin::decodePlan(R"({"location":{"name":"Кабинет 1","type":"office","description":"Офисный кабинет.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},"devices":[{"name":"Свет кабинета 1","template":"light","aliases":["свет кабинета 1"],"description":"Освещение кабинета."},{"name":"Телевизор кабинета 1","template":"tv","aliases":["телевизор кабинета 1"],"description":"Экран кабинета."},{"name":"Кондиционер кабинета 1","template":"switch","aliases":["кондиционер кабинета 1"],"description":"Виртуальный климат."}]})",catalog));
        check(!officePlan.at("saved").get<bool>()&&!officePlan.at("applied").get<bool>(),"Office plan remains editable draft");
        check(officePlan.at("configuration").at("devices").size()==catalog->configuration().devices.size()+3,"Office plan expands to three ordinary devices");
        const auto officeCatalog=config::ConfigLoader::fromJson(officePlan.at("configuration").dump());
        runtime::DomainView officeDomain(officeCatalog);
        const auto officeId=officePlan.at("location_id").get<std::string>();
        check(officeDomain.resolve("Включи свет в кабинете 1").locations==std::vector<std::string>{officeId},"Natural location phrase selects office");
        check(officeDomain.resolve("кабинет 1").locations==std::vector<std::string>{officeId},"Natural mention selects office");
        check(officeDomain.resolve("на кухне").locations==std::vector<std::string>{"room_kitchen"},"Kitchen location still resolves");
        auto clash=Json::parse(officePlan.at("configuration").dump());
        clash["rooms"].push_back({{"id","location_office_dup"},{"name","Кабинет дубль"},{"speech",{{"mentions",{"кабинет дубль"}},{"locations",{"в кабинете 1"}}}},
            {"metadata",{{"type","office"},{"description","Конфликт."},{"aliases",Json::array()},{"parent_id",nullptr},{"container",false}}}});
        rejects([&]{ (void)config::ConfigLoader::fromJson(clash.dump()); });
        rejects([&]{admin::decodePlan(R"({"locations":[{"name":"A"},{"name":"B"}],"devices":[]})",catalog);});
        const auto beforeDoor=catalog->configuration().devices.size();
        bool entranceSwitchRejected=false; std::string entranceSwitchReason;
        try {
            (void)admin::decodePlan(R"({"location":{"name":"Кабинет 1","type":"office","description":"Офис.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},"devices":[{"name":"Входная дверь","template":"switch","aliases":["входная дверь"],"description":"Дверь на входе."}]})",catalog);
        } catch (const std::exception& error) {
            entranceSwitchRejected=true; entranceSwitchReason=error.what();
        }
        check(entranceSwitchRejected,"Entrance door must not become a generic switch device");
        check(entranceSwitchReason.find("Protected entrance door")!=std::string::npos,"Controlled unsupported for entrance door as switch");
        check(catalog->configuration().devices.size()==beforeDoor,"Rejected entrance-door plan creates no devices");
        rejects([&]{admin::decodePlan(R"({"location":{"name":"Кабинет 1","type":"office","description":"Офис.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},"devices":[{"name":"Дверь","template":"door","aliases":["дверь"],"description":"Вход."}]})",catalog);});
        // Ordinary switch remains allowed (relay/AC), including a non-entrance zone door label.
        (void)admin::decodePlan(R"({"location":{"name":"Кладовая","type":"storage","description":"Склад.","mentions":["кладовая"],"locations":["в кладовой"]},"devices":[{"name":"Реле 1","template":"switch","aliases":["реле 1"],"description":"Питание."},{"name":"Дверь кладовой","template":"switch","aliases":["дверь кладовой"],"description":"Зонная дверь."}]})",catalog);

        const auto customSuggestion=Json::parse(admin::decodeActionDraft(R"({"description":"Задать метку образца.","aliases":["пометь образец"],"assignments":[{"field":"label","argument":true}]})",customCatalog,"device_sample","Задать метку"));
        check(customSuggestion.at("parameters")[0].at("type")=="string"&&customSuggestion.at("assignments")[0].at("argument")=="label","AI action inherits typed state constraints");
        rejects([&]{admin::decodeActionDraft(R"({"description":"Открыть","aliases":["открой"],"assignments":[{"field":"state","value":"OPEN"}]})",catalog,"device_entrance_door","Открыть без проверки");});
        auto inventory=document;inventory["devices"].back()["state"]=Json::array();inventory["devices"].back()["action_ids"]=Json::array();
        execution::MockDeviceExecutor oldInventory(config::ConfigLoader::fromJson(inventory.dump()));execution::MockDeviceExecutor activated(catalog);activated.reconcile(oldInventory.states());
        check(activated.preview(on).success,"Inventory gains explicitly initialized state without losing stable ID");
        std::string scratchTemplate=(std::filesystem::temp_directory_path()/"firefly-v2-store-XXXXXX").string();
        const auto* allocated=::mkdtemp(scratchTemplate.data());if(!allocated)throw std::runtime_error("mkdtemp failed");const std::filesystem::path scratch(allocated);
        std::filesystem::permissions(scratch,std::filesystem::perms::owner_all);
        {config::ConfigStore store(scratch.string());const auto stored=store.initialize(old);const auto saved=store.save(catalog,{stored->configuration().revision,stored->contentHash()});
         check(saved->configuration().schemaVersion==2&&store.current()->contentHash()==saved->contentHash(),"v1 store migrates through normal CAS save");
         check(store.revision(1)->contentHash()==old->contentHash(),"Historical v1 backup reload unchanged");
         const auto restored=store.restoreCandidate(1,{saved->configuration().revision,saved->contentHash()});check(restored->configuration().schemaVersion==1&&store.current()->contentHash()==saved->contentHash(),"Restore remains candidate only across schema versions");}
        std::filesystem::remove_all(scratch);
        std::cout << "TypedMock: " << checks << " checks passed\n";return 0;
    } catch(const std::exception& e){std::cerr << "TypedMock after " << checks << ": " << e.what() << '\n';return 1;}
}
