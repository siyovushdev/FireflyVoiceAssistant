#include "Support.h"
#include "DialogueOperations.h"
#include "SetupSession.h"
#include <iostream>

using namespace firefly::admin;
using admin_test::Json;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class Model final : public admin_test::Model {
public:
    std::vector<std::string> replies;
    std::size_t next = 0;
    bool setupRequired() const override { return true; }
    void enterSetup() override {}
    void leaveSetup(const std::string& value) override { useContext(value); }
    void useAdminContext(const std::string& value, int) override { prompt = value; }
    void script(std::vector<std::string> values) { replies = std::move(values); next = 0; }
    std::string inferAdmin(std::string_view) override {
        ++inferences;
        // Language enrichment is separate and may fail without losing a valid plan.
        return next < replies.size() ? replies[next++] : "{}";
    }
};
const std::string ownerPhrase = "создай цех зону и добавь туда вентилятор где он может включаться и выключаться по питанию";
const std::string valid = R"({"ops":[["location","Цех"],["power_device","Цех","Вентилятор"]]})";
void powerDevice(const Json& config, const std::string& zone, const std::string& name) {
    require(config.at("rooms").size() == 1 && config.at("rooms")[0].at("name") == zone, "exact requested zone");
    require(config.at("devices").size() == 1, "exactly one device");
    const auto& device = config.at("devices")[0];
    require(device.at("name") == name && device.at("room_id") == config.at("rooms")[0].at("id"), "device reference resolves newly created zone");
    require(device.at("action_ids").size() == 2 && device.at("state").size() == 1, "power only, no invented capabilities");
    std::set<bool> power;
    for (const auto& action : config.at("actions")) {
        require(action.at("parameters").empty() && action.at("assignments").size() == 1, "no action arguments");
        require(action.at("assignments")[0].at("field") == "power", "only power assignment");
        power.insert(action.at("assignments")[0].at("value").get<bool>());
    }
    require(power == std::set<bool>{false, true}, "exact ON/OFF result states");
    require(config.at("bindings").empty(), "semantic proposal has no hardware mappings");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        admin_test::Directory directory;
        firefly::config::ConfigStore store(directory.path);
        const auto baseline = store.initialize(firefly::config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/empty-home.json"));
        const auto empty = Json::parse(baseline->canonicalJson());
        Model model;
        RuntimeWorker worker(model);
        Json lastDialogue;
        const auto call = [&](const Json& request) {
            auto response = Json::parse(worker.handle(request.dump()));
            if (request.at("method") == "dialogue") lastDialogue = response;
            return response;
        };
        require(call({{"method", "apply"}, {"configuration", empty}}).value("ok", false), "initial active configuration");
        const auto activeBefore = Json::parse(worker.observe()).at("active");
        SetupSession session(store, call);
        auto state = session.handle("start", {});
        auto token = state.at("session_id");
        int sequence = 0;
        const auto message = [&](const std::string& text) {
            state = session.handle("message", {{"session_id", token}, {"version", state.at("version")},
                {"request_id", std::to_string(++sequence)}, {"atomic_message", true}, {"text", text}});
        };
        const auto reset = [&] {
            state = session.handle("draft", {{"session_id", token}, {"version", state.at("version")}, {"configuration", empty}});
        };
        const auto unchanged = [&] {
            require(store.current()->contentHash() == baseline->contentHash(), "stored config unchanged");
            require(Json::parse(worker.observe()).at("active") == activeBefore, "active config unchanged");
            require(Json::parse(worker.observe()).at("executions").empty(), "zero executions");
        };
        for (const auto& text : {ownerPhrase, std::string("В цехе есть вентилятор, его можно включать и выключать.")}) {
            model.script({valid}); message(text);
            require(state.at("status") == "editing", "named new device with same-message pronoun reaches decoder");
            powerDevice(state.at("configuration"), "Цех", "Вентилятор");
            require(lastDialogue.at("trace").size() == 1, "one validated generation");
            unchanged(); reset();
        }
        // Same meaning with different entities: no special case for owner wording.
        model.script({R"({"ops":[["location","Лаборатория"],["power_device","Лаборатория","Насос"]]})"});
        message("В лаборатории есть насос, его можно включать и выключать.");
        powerDevice(state.at("configuration"), "Лаборатория", "Насос"); unchanged(); reset();
        model.script({R"({"ops":[["device",null,"Серверный ИБП"]]})"});
        message("Добавь серверный ИБП без зоны.");
        const auto& noZone = state.at("configuration");
        require(noZone.at("rooms").empty() && noZone.at("devices").size() == 1 && noZone.at("devices")[0].at("room_id").is_null(), "explicit no-zone without fake zone");
        require(noZone.at("actions").empty() && noZone.at("devices")[0].at("state").empty(), "unspecified capabilities not invented");
        unchanged(); reset();

        const auto trailing = valid + "\n\n" + R"({"unsupported":"лишнее"})";
        const std::vector<std::string> invalid = {
            "{broken", valid + "}", trailing, valid + " пояснение", "```json\n" + valid + "\n```",
            R"({"ops":[["invent_operation","Цех"]]})",
            R"({"ops":[["power_device","Цех","Вентилятор","extra"]]})",
            R"({"ops":[["location","Цех"],["location","Цех"]]})"
        };
        for (const auto& raw : invalid) {
            // Keep a good manual pending task while the AI request fails.
            const auto manual = Json::parse(decodeCopilotProposal(expandDialogueOperations(R"({"ops":[["location","Склад"]]})").dump(), baseline)).at("configuration");
            state = session.handle("draft", {{"session_id", token}, {"version", state.at("version")}, {"configuration", manual}});
            model.script({raw, raw});
            bool rejected = false;
            try { message(ownerPhrase); } catch (const std::exception&) { rejected = true; }
            state = session.handle("status", {});
            require(rejected && state.at("status") == "planning_failed", "both invalid retain safe failure state");
            require(state.at("configuration") == manual, "failed AI preserves good manual pending task");
            require(lastDialogue.at("trace").size() == 2 && !lastDialogue.value("ok", true), "exactly one retry, no permissive parse");
            require(lastDialogue.at("trace")[0].at("raw") == raw && lastDialogue.at("trace")[1].at("raw") == raw, "raw bytes never repaired");
            unchanged();
            session.handle("cancel", {{"session_id", token}});
            state = session.handle("start", {}); token = state.at("session_id");
        }
        model.script({trailing, valid}); message(ownerPhrase);
        powerDevice(state.at("configuration"), "Цех", "Вентилятор");
        require(lastDialogue.at("retry_count") == 1 && lastDialogue.at("trace")[0].contains("error") && lastDialogue.at("trace")[1].contains("decoded"), "only valid second output becomes proposal");
        unchanged(); reset();
        model.script({R"({"ops":[["power_device","Несуществующая зона","Вентилятор"]]})"});
        message(ownerPhrase);
        require(state.at("status") == "clarification_required" && state.at("configuration") == empty,
            "unknown reference keeps checkpoint and asks instead of proposing a fake zone");
        unchanged(); reset();
        model.script({valid});
        auto ambiguous = call({{"method", "dialogue"}, {"configuration", empty}, {"focus", Json::array()}, {"text", "Переименуй его."}});
        require(ambiguous.at("suggestion").at("status") == "ambiguous" && model.next == 0, "unresolved edit still asks before inference");
        model.script({R"({"ops":[["bounds","Цех","Вентилятор","speed",0,100]]})"});
        ambiguous = call({{"method", "dialogue"}, {"configuration", empty}, {"focus", Json::array()}, {"text", "Измени его диапазон от 0 до 100."}});
        require(ambiguous.at("suggestion").at("status") == "ambiguous" && !ambiguous.at("suggestion").contains("configuration"), "deferred pronoun check cannot authorize existing-entity changes");
        model.script({R"({"ops":[["location","Цех"],["power_device","Цех","Насос"],["power_device","Цех","Вентилятор"]]})"});
        ambiguous = call({{"method", "dialogue"}, {"configuration", empty}, {"focus", Json::array()}, {"text", "В цехе есть насос и вентилятор, его можно включать и выключать."}});
        require(ambiguous.at("suggestion").at("status") == "ambiguous" && !ambiguous.at("suggestion").contains("configuration"), "pronoun with multiple proposed devices remains ambiguous");
        unchanged();
        session.handle("cancel", {{"session_id", token}});
        std::cout << "PASS dialogue A-G: exact and paraphrased power, no-zone, strict output, retry, manual checkpoint, unchanged active/store, zero execution\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
