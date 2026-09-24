#include "../admin/Support.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/config/ConfigValidator.h"
#include <iostream>

using namespace admin_test;
namespace {
int checks = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
template<class F>
void rejects(F fn, const char* description) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    check(failed, description);
}
std::string officeCreateRaw() {
    return R"({
      "status":"ok",
      "summary":"Будет создан Кабинет 1 со светом, кондиционером и телевизором.",
      "operations":[
        {"op":"create_location","name":"Кабинет 1","type":"office","description":"Офисный кабинет.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},
        {"op":"create_device","location":"Кабинет 1","name":"Свет кабинета 1","template":"light","aliases":["свет кабинета 1"],"description":"Освещение."},
        {"op":"create_device","location":"кабинет 1","name":"Кондиционер","template":"switch","aliases":["кондиционер"],"description":"Климат."},
        {"op":"create_device","location":"в кабинете 1","name":"Телевизор","template":"tv","aliases":["телевизор"],"description":"Экран."}
      ]
    })";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto baseline = base(argv[1]);
        const auto snap = catalog(baseline);
        const auto baseHash = firefly::admin::draftFingerprint(snap);

        const auto created = Json::parse(firefly::admin::decodeCopilotProposal(officeCreateRaw(), snap));
        check(created.at("saved") == false && created.at("applied") == false, "Proposal never saves/applies");
        check(created.at("base_hash") == baseHash, "Proposal binds to draft fingerprint");
        check(created.at("configuration").at("rooms").size() == snap->configuration().rooms.size() + 1, "Office location added");
        check(created.at("changes").is_array() && !created.at("changes").empty(), "Structural changes present");
        bool hasTv = false, hasAc = false, hasLight = false;
        for (const auto& device : created.at("configuration").at("devices")) {
            if (device.at("name") == "Телевизор") hasTv = true;
            if (device.at("name") == "Кондиционер") hasAc = true;
            if (device.at("name") == "Свет кабинета 1") hasLight = true;
        }
        check(hasTv && hasAc && hasLight, "Office plan creates light, AC and TV");

        auto manual = created.at("configuration");
        std::string officeId;
        for (const auto& room : manual.at("rooms"))
            if (room.at("name") == "Кабинет 1") officeId = room.at("id").get<std::string>();
        check(!officeId.empty(), "Office id resolved after create");
        manual["devices"].push_back({{"id", "device_manual_fan"}, {"name", "Вентилятор ручной"}, {"kind", "generic"},
            {"room_id", officeId}, {"enabled", true}, {"action_ids", Json::array()},
            {"spoken_aliases", {"вентилятор ручной"}}, {"state", Json::array()},
            {"metadata", {{"type", "switch"}, {"description", "Ручное"}, {"aliases", {"вентилятор ручной"}},
                {"origin", "manual"}, {"confirmed", true}}}});
        const auto fanKept = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Добавится лампа.","operations":[{"op":"create_device","location":"Кабинет 1","name":"Лампа","template":"light","aliases":["лампа"],"description":"Свет."}]})",
            catalog(manual)));
        bool fanPresent = false;
        for (const auto& device : fanKept.at("configuration").at("devices"))
            if (device.at("id") == "device_manual_fan") fanPresent = true;
        check(fanPresent, "Manual unsaved device survives Copilot proposal");

        const auto livingAdd = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Добавится телевизор в гостиную.","operations":[{"op":"create_device","location":"гостиной","name":"Телевизор гостиной","template":"tv","aliases":["телевизор гостиной"],"description":"Экран."}]})",
            snap));
        check(livingAdd.at("configuration").at("devices").size() == snap->configuration().devices.size() + 1,
            "Add TV to living room");
        bool livingTv = false;
        for (const auto& device : livingAdd.at("configuration").at("devices"))
            if (device.at("name") == "Телевизор гостиной") {
                livingTv = true;
                check(device.at("room_id") == "room_living", "TV attached to living room");
            }
        check(livingTv, "Living-room TV created");

        const auto volume = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Громкость телевизора 0..80.","operations":[{"op":"set_field_bounds","device":"Телевизор гостиной","location":"Гостиная","field":"volume","minimum":0,"maximum":80}]})",
            catalog(livingAdd.at("configuration"))));
        check(!volume.at("changes").empty(), "Volume bounds change recorded");
        bool volumeBound = false;
        for (const auto& device : volume.at("configuration").at("devices"))
            if (device.at("name") == "Телевизор гостиной")
                for (const auto& field : device.at("state"))
                    if (field.at("id") == "volume") volumeBound = field.at("maximum") == 80;
        check(volumeBound, "Volume maximum updated to 80");

        // Two locations + devices in one proposal apply atomically.
        const auto dual = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Кабинет и Лаборатория.","operations":[
              {"op":"create_location","name":"Кабинет M12","type":"office","description":"Офис.","mentions":["кабинет m12"],"locations":["в кабинете m12"]},
              {"op":"create_device","location":"Кабинет M12","name":"Свет кабинета M12","template":"light","aliases":["свет кабинета m12"],"description":"Свет."},
              {"op":"create_location","name":"Лаборатория M12","type":"lab","description":"Лаборатория.","mentions":["лаборатория m12"],"locations":["в лаборатории m12"]},
              {"op":"create_device","location":"Лаборатория M12","name":"Регулятор лаборатории","template":"numeric","aliases":["регулятор лаборатории"],"description":"Число."}
            ]})", snap));
        check(dual.at("saved") == false && dual.at("applied") == false, "Dual proposal never saves/applies");
        bool officeM12 = false, labM12 = false, officeLight = false, labNumeric = false;
        for (const auto& room : dual.at("configuration").at("rooms")) {
            if (room.at("name") == "Кабинет M12") officeM12 = true;
            if (room.at("name") == "Лаборатория M12") labM12 = true;
        }
        for (const auto& device : dual.at("configuration").at("devices")) {
            if (device.at("name") == "Свет кабинета M12") officeLight = true;
            if (device.at("name") == "Регулятор лаборатории") labNumeric = device.at("kind") == "generic";
        }
        check(officeM12 && labM12 && officeLight && labNumeric, "Two locations and devices applied atomically");

        // Number field bounds accept fractional maximum; action parameter stays synced.
        const auto numberBounds = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Диапазон 0..12.5.","operations":[{"op":"set_field_bounds","device":"Регулятор лаборатории","location":"Лаборатория M12","field":"value","minimum":0,"maximum":12.5}]})",
            catalog(dual.at("configuration"))));
        bool fieldOk = false, paramOk = false;
        for (const auto& device : numberBounds.at("configuration").at("devices")) {
            if (device.at("name") != "Регулятор лаборатории") continue;
            for (const auto& field : device.at("state"))
                if (field.at("id") == "value")
                    fieldOk = field.at("minimum").get<double>() == 0.0 && field.at("maximum").get<double>() == 12.5;
            for (const auto& actionId : device.at("action_ids"))
                for (const auto& action : numberBounds.at("configuration").at("actions"))
                    if (action.at("id") == actionId)
                        for (const auto& parameter : action.at("parameters"))
                            if (parameter.at("id") == "value")
                                paramOk = parameter.at("minimum").get<double>() == 0.0 &&
                                          parameter.at("maximum").get<double>() == 12.5;
        }
        check(fieldOk && paramOk, "Number bounds 0..12.5 sync field and parameter");
        rejects([&] {
            (void)firefly::admin::decodeCopilotProposal(
                R"({"status":"ok","summary":"Дробные границы для громкости.","operations":[{"op":"set_field_bounds","device":"Телевизор гостиной","location":"Гостиная","field":"volume","minimum":0,"maximum":12.5}]})",
                catalog(livingAdd.at("configuration")));
        }, "Integer field rejects fractional bounds");

        // Two equally matching devices without location → deterministic ambiguous reject.
        const auto twinTvs = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Два телевизора.","operations":[
              {"op":"create_device","location":"Кухня","name":"Телевизор","template":"tv","aliases":["телевизор"],"description":"Кухонный ТВ."},
              {"op":"create_device","location":"Гостиная","name":"Телевизор","template":"tv","aliases":["телевизор"],"description":"Гостиный ТВ."}
            ]})", snap));
        rejects([&] {
            (void)firefly::admin::decodeCopilotProposal(
                R"({"status":"ok","summary":"Удалить телевизор.","operations":[{"op":"delete_device","device":"Телевизор","location":null}]})",
                catalog(twinTvs.at("configuration")));
        }, "Ambiguous dual device rejected deterministically");

        const auto nursery = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Детская.","operations":[{"op":"create_location","name":"Детская","type":"room","description":"Комната.","mentions":["детская"],"locations":["в детской"]}]})",
            snap));
        const auto renamed = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Детская станет Комнатой детей.","operations":[{"op":"rename_location","location":"Детская","name":"Комната детей","keep_previous_as_alias":true}]})",
            catalog(nursery.at("configuration"))));
        bool renamedOk = false;
        for (const auto& room : renamed.at("configuration").at("rooms")) if (room.at("name") == "Комната детей") {
            bool alias = false;
            for (const auto& mention : room.at("speech").at("mentions")) if (mention == "детская") alias = true;
            check(alias, "Previous name kept as mention alias");
            renamedOk = true;
        }
        check(renamedOk, "Rename target found");

        // Prompt contract no longer advertises ensure_integer_field.
        const auto prompt = firefly::admin::copilotPrompt(snap);
        check(prompt.find("ensure_integer_field") == std::string::npos, "Prompt omits ensure_integer_field");
        check(prompt.find("\"op\":\"create_device\"") != std::string::npos, "Prompt shows canonical op discriminator");
        check(prompt.find("rename_location{") != std::string::npos, "Prompt lists rename_location contract");
        check(prompt.find("set_field_bounds{") != std::string::npos, "Prompt lists set_field_bounds contract");

        // «добавь телевизор в гостиную»: only create_device; existing location speech unchanged.
        const auto migratedBaseline = Json::parse(
            firefly::config::ConfigValidator::validate(firefly::config::migrateV2(snap->configuration()))->canonicalJson());
        Json livingSpeechBefore;
        for (const auto& room : migratedBaseline.at("rooms"))
            if (room.at("name") == "Гостиная") livingSpeechBefore = room.at("speech");
        check(!livingSpeechBefore.is_null(), "Living speech captured from migrated baseline");
        const auto livingTvOnly = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Телевизор в гостиной.","operations":[{"op":"create_device","location":"Гостиная","name":"Телевизор","template":"tv","description":"ТВ.","aliases":["телевизор"]}]})",
            snap));
        check(livingTvOnly.at("operations").size() == 1 && livingTvOnly.at("operations")[0].at("op") == "create_device",
            "Living-room TV: only create_device");
        bool livingTvPresent = false;
        for (const auto& device : livingTvOnly.at("configuration").at("devices"))
            if (device.at("name") == "Телевизор" && device.at("room_id") == "room_living") livingTvPresent = true;
        check(livingTvPresent, "TV attached to living room");
        for (const auto& room : livingTvOnly.at("configuration").at("rooms"))
            if (room.at("name") == "Гостиная") {
                check(room.at("speech").at("mentions") == livingSpeechBefore.at("mentions"), "Living mentions unchanged");
                check(room.at("speech").at("locations") == livingSpeechBefore.at("locations"), "Living phrases unchanged");
            }
        for (const auto& change : livingTvOnly.at("changes"))
            check(change.at("entity") != "location", "No location speech diff for device-only create");

        // Side-effect speech ops with create_device are rejected.
        const auto mixedSpeech = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"ТВ.","operations":[
              {"op":"create_device","location":"Гостиная","name":"Телевизор 2","template":"tv","description":"ТВ.","aliases":["телевизор 2"]},
              {"op":"add_location_mention","location":"Гостиная","mention":"в гостиной"},
              {"op":"add_location_phrase","location":"Гостиная","phrase":"в гостиной"}
            ]})", snap));
        check(mixedSpeech.at("rejected") == true && mixedSpeech.at("status") == "unsupported",
            "create_device+speech edits rejected");
        check(!mixedSpeech.contains("configuration"), "Mixed speech rejection does not mutate config");

        // Exact duplicate phrase alone → no changes → rejected by decoder.
        rejects([&] {
            (void)firefly::admin::decodeCopilotProposal(
                R"({"status":"ok","summary":"Дубль.","operations":[{"op":"add_location_phrase","location":"Гостиная","phrase":"в гостиной"}]})",
                snap);
        }, "Exact duplicate phrase produces no changes");

        // Missing location: controlled unknown_location, no silent create.
        const auto missingNursery = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"ТВ в детской.","operations":[{"op":"create_device","location":"Детская комната","name":"Телевизор","template":"tv","description":"ТВ.","aliases":["телевизор"]}]})",
            snap));
        check(missingNursery.at("rejected") == true && missingNursery.at("code") == "unknown_location",
            "Unknown nursery returns unknown_location");
        check(std::string(missingNursery.at("summary").get<std::string>()).find("Детская комната") != std::string::npos,
            "Unknown location names the missing zone");
        check(!missingNursery.contains("configuration"), "Unknown location does not mutate config");

        // Explicit create location + device remains atomic.
        const auto nurseryCreate = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Детская и ТВ.","operations":[
              {"op":"create_location","name":"Детская комната","type":"room","description":"Детская.","mentions":["детская комната","детская"],"locations":["в детской комнате","в детской"]},
              {"op":"create_device","location":"Детская комната","name":"Телевизор детской","template":"tv","description":"ТВ.","aliases":["телевизор детской"]}
            ]})", snap));
        bool nurseryRoom = false, nurseryTv = false;
        for (const auto& room : nurseryCreate.at("configuration").at("rooms"))
            if (room.at("name") == "Детская комната") nurseryRoom = true;
        for (const auto& device : nurseryCreate.at("configuration").at("devices"))
            if (device.at("name") == "Телевизор детской") nurseryTv = true;
        check(nurseryRoom && nurseryTv, "Explicit nursery+TV created atomically");
        check(nurseryCreate.at("operations").size() == 2, "Nursery create keeps both ops");

        rejects([&] {
            (void)firefly::admin::decodeCopilotProposal(
                R"({"status":"ok","summary":"bad","operations":[{"create_device":{"location":"Гостиная","name":"ТВ","template":"tv","description":"x"}}]})",
                snap);
        }, "Nested create_device without op discriminator rejected");

        const auto deleted = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Телевизор гостиной будет удалён.","operations":[{"op":"delete_device","device":"Телевизор гостиной","location":"Гостиная"}]})",
            catalog(livingAdd.at("configuration"))));
        for (const auto& device : deleted.at("configuration").at("devices"))
            check(device.at("name") != "Телевизор гостиной", "Deleted TV absent");
        bool removalVisible = false;
        for (const auto& change : deleted.at("changes"))
            if (change.at("change") == "removed" && change.at("name") == "Телевизор гостиной") removalVisible = true;
        check(removalVisible, "Deletion visible in diff");

        const auto ambiguous = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ambiguous","summary":"Уточните, какой телевизор удалить.","operations":[]})", snap));
        check(ambiguous.at("rejected") == true && ambiguous.at("status") == "ambiguous", "Ambiguous returns controlled rejection");
        check(!ambiguous.contains("configuration"), "Ambiguous does not modify configuration");

        rejects([&] {
            (void)firefly::admin::decodeCopilotProposal(
                R"({"status":"ok","summary":"Входная дверь.","operations":[{"op":"create_device","location":"Кухня","name":"Входная дверь","template":"switch","aliases":["входная дверь"],"description":"Дверь на входе."}]})",
                snap);
        }, "Entrance door as switch rejected");
        // Incomplete LLM location fields get deterministic speech defaults; unknown AC template maps to switch.
        const auto incomplete = Json::parse(firefly::admin::decodeCopilotProposal(
            R"({"status":"ok","summary":"Кабинет.","operations":[{"op":"create_location","name":"Кабинет 2","type":"office","description":"Офис."},{"op":"create_device","location":"Кабинет 2","name":"Кондиционер","template":"air_conditioner","description":"Климат."}]})",
            snap));
        bool office2 = false, acSwitch = false;
        for (const auto& room : incomplete.at("configuration").at("rooms")) if (room.at("name") == "Кабинет 2") {
            office2 = !room.at("speech").at("mentions").empty() && !room.at("speech").at("locations").empty();
        }
        for (const auto& device : incomplete.at("configuration").at("devices"))
            if (device.at("name") == "Кондиционер") acSwitch = device.at("metadata").at("type") == "switch" || device.at("kind") == "generic";
        check(office2 && acSwitch, "Defaults and template aliasing accept incomplete Copilot JSON");
        rejects([&] { (void)firefly::admin::decodeCopilotProposal("{", snap); }, "Malformed JSON rejected");

        const auto accepted = Json::parse(firefly::admin::acceptCopilotProposal(snap->canonicalJson(), created.dump()));
        check(accepted.at("ok") == true && accepted.at("saved") == false && accepted.at("applied") == false, "Accept updates draft only");
        rejects([&] {
            auto staleDraft = Json::parse(snap->canonicalJson());
            staleDraft["system"]["name"] = "Changed";
            (void)firefly::admin::acceptCopilotProposal(staleDraft.dump(), created.dump());
        }, "Stale proposal rejected");

        class ScriptedModel final : public Model {
        public:
            std::vector<std::string> responses, prompts;
            std::size_t index = 0;
            void useAdminContext(const std::string& value, int) override { useContext(value); prompts.push_back(value); }
            std::string infer(std::string_view) override {
                ++inferences;
                if (index >= responses.size()) throw std::runtime_error("Unexpected extra inference");
                return responses[index++];
            }
            std::string inferenceMetrics() const override {
                return Json{{"context_tokens", 400}, {"input_tokens", 20}, {"output_tokens", 120},
                    {"output_budget", 384}, {"wall_ms", 15}, {"finish", "ok"}}.dump();
            }
            void script(std::vector<std::string> values) { responses = std::move(values); index = 0; prompts.clear(); }
        };
        ScriptedModel model;
        firefly::admin::RuntimeWorker worker(model);
        firefly::admin::LocalRuntimeClient client(worker);
        Directory directory;
        firefly::config::ConfigStore store(directory.path);
        store.initialize(snap);
        firefly::admin::Api api(store, client);
        api.initializeRuntime();
        const auto call = [&](const std::string& path, Json body) {
            const auto response = api.handle("POST", path, body.dump());
            return std::pair<int, Json>{response.status, Json::parse(response.body)};
        };
        const auto originalHash = store.current()->contentHash();
        model.script({officeCreateRaw() + "}", officeCreateRaw()});
        auto reply = call("/api/admin/copilot",
            {{"text", "Создай кабинет 1 и добавь свет, кондиционер и телевизор."}, {"configuration", baseline}});
        check(reply.first == 200 && reply.second.at("ok") == true, "Copilot HTTP succeeds after retry");
        check(reply.second.at("retry_count") == 1, "One retry for malformed then valid");
        check(reply.second.at("draft").at("saved") == false && reply.second.at("draft").at("applied") == false, "HTTP draft markers");
        check(store.current()->contentHash() == originalHash, "Copilot does not save");
        const auto proposal = reply.second.at("draft");
        auto accept = call("/api/admin/copilot/accept", {{"configuration", baseline}, {"proposal", proposal}});
        check(accept.first == 200 && accept.second.at("saved") == false, "Accept only returns draft");
        check(store.current()->contentHash() == originalHash, "Accept does not save");
        auto changed = baseline;
        changed["system"]["name"] = "Other";
        accept = call("/api/admin/copilot/accept", {{"configuration", changed}, {"proposal", proposal}});
        check(accept.first == 422 && accept.second.at("errors")[0].at("code") == "stale_proposal", "HTTP stale reject");

        model.script({R"({"status":"ambiguous","summary":"Уточните телевизор.","operations":[]})"});
        reply = call("/api/admin/copilot", {{"text", "Удали телевизор"}, {"configuration", baseline}});
        check(reply.first == 422 && reply.second.at("errors")[0].at("code") == "ambiguous", "Ambiguous HTTP controlled error");

        std::cout << "AdminCopilot: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AdminCopilot failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
