#include "Support.h"
#include "DialogueOperations.h"
#include "SetupSession.h"
#include <iostream>
#include <sys/stat.h>

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
            std::ifstream saved(directory.path+"/setup-draft.json");
            const auto disk=Json::parse(saved);
            for(const auto& entry:disk.at("trace"))if(!entry.contains("error_code"))
                require(!entry.contains("rejected_raw_hex"), "successful raw responses remain excluded from the private trace");
            unchanged(); reset();
        }
        // Exact long stress input, with an explicitly scripted VALID response:
        // checks the real request/decoder contract, not Qwen acceptance.
        std::string longRequest="Создай зону Кабинет и лампу в кабинете, которую можно включать и выключать";
        const std::string detail=" Требуется только питание лампы в кабинете, включение и выключение, без яркости, каналов и других возможностей, без подключения к сети";
        while(longRequest.size()+detail.size()<=1800)longRequest+=detail;
        require(longRequest.size()==1606,"same long UTF-8 stress input");
        model.script({R"({"ops":[["location","Кабинет"],["power_device","Кабинет","Лампа"]]})"});
        message(longRequest);powerDevice(state.at("configuration"),"Кабинет","Лампа");unchanged();reset();
        // Exact raw responses captured from the real 2026-10-15 diagnostic
        // request. Offline fixtures verify rejection/retry, not Qwen quality.
        const std::string malformedFirst=R"({"ops":[{"system_name","Кабинет"},{"location","Кабинет"},{"power_device","Кабинет","Лампа в кабинете"}]})";
        const std::string malformedSecond=R"({"ops":[{"location","Кабинет"},{"power_device","Кабинет","Лампа в кабинете"}]})";
        model.script({malformedFirst,malformedSecond});
        bool malformedRejected=false;
        try {message(longRequest);}catch(const std::exception&){malformedRejected=true;}
        state=session.handle("status",{});
        require(malformedRejected && state.at("status")=="planning_failed" && state.at("message_incomplete")==true,
            "captured invalid tuples fail closed on exact long input");
        require(state.at("configuration")==empty && lastDialogue.at("trace").size()==2 && model.next==2,
            "invalid tuples never become a proposal or trigger extra retries");
        require(lastDialogue.at("trace")[0].at("raw")==malformedFirst && lastDialogue.at("trace")[1].at("raw")==malformedSecond,
            "captured Qwen bytes are rejected without repair");
        for(const auto& entry:lastDialogue.at("trace")) {
            require(entry.at("rejection_stage")=="json_parse","captured tuples fail syntax before semantic decode");
            const auto instructions=entry.at("prompt").get<std::string>();
            const auto begin=instructions.find("{\"ops\":");
            require(begin!=std::string::npos,"dialogue instructions contain a complete plan example");
            const auto end=instructions.find('}',begin);
            const auto example=expandDialogueOperations(instructions.substr(begin,end-begin+1));
            require(example.at("operations").size()==2,"instruction example is valid JSON with typed operation arrays");
        }
        require(lastDialogue.at("trace")[1].at("prompt").get<std::string>().find("квадратные скобки")!=std::string::npos,
            "format retry explains JSON array brackets");
        unchanged();session.handle("cancel",{{"session_id",token}});
        state=session.handle("start",{});token=state.at("session_id");
        model.script({malformedSecond,R"({"ops":[["location","Кабинет"],["power_device","Кабинет","Лампа"]]})"});
        message(longRequest);powerDevice(state.at("configuration"),"Кабинет","Лампа");
        require(lastDialogue.at("retry_count")==1 && lastDialogue.at("trace")[0].at("raw")==malformedSecond &&
            lastDialogue.at("trace")[1].contains("decoded"),"only a separately generated valid array response succeeds");
        unchanged();reset();
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
            R"({"ops":[["location","Цех"],["location","Цех"]]})",
            std::string(4095,'x')+"Ж" // The 4096-byte capture boundary splits UTF-8.
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
            std::ifstream saved(directory.path+"/setup-draft.json");
            const auto disk=Json::parse(saved);
            const auto& traces=disk.at("trace");
            std::string expectedHex;
            for(std::size_t i=0;i<std::min(raw.size(),std::size_t(4096));++i) {
                const auto byte=static_cast<unsigned char>(raw[i]);
                expectedHex.push_back("0123456789abcdef"[byte>>4]);expectedHex.push_back("0123456789abcdef"[byte&15]);
            }
            for(std::size_t i=traces.size()-2;i<traces.size();++i) {
                const auto& entry=traces.at(i);
                require(entry.at("error_code")=="invalid_ai_response" && !entry.at("error").get<std::string>().empty(), "private trace retains concrete rejection reason");
                require(entry.at("rejected_raw_hex")==expectedHex && entry.at("rejected_raw_bytes")==raw.size() &&
                        entry.at("rejected_raw_truncated")==bool(raw.size()>4096), "failed raw prefix is exact, bounded and explicitly labelled");
                if(lastDialogue.at("trace").at(i-(traces.size()-2)).contains("rejection_stage"))
                    require(entry.at("rejection_stage")=="json_parse", "parser stage retained");
            }
            struct stat info{};
            require(::stat((directory.path+"/setup-draft.json").c_str(),&info)==0 && (info.st_mode&0777)==0600, "diagnostic draft stays private");
            require(!state.contains("trace") && state.dump().find("rejected_raw_hex")==std::string::npos, "diagnostics never enter public setup status");
            unchanged();
            session.handle("cancel", {{"session_id", token}});
            state = session.handle("start", {}); token = state.at("session_id");
        }
        // Repeated failed requests must not exhaust storage and block restore.
        for(int i=0;i<32;++i) {
            model.script({std::string(5000,'x'),std::string(5000,'x')});
            bool rejected=false;
            try {message(ownerPhrase);}catch(const std::exception& error) {
                rejected=true;
                require(std::string(error.what()).find("setup_private_storage_limit")==std::string::npos,"diagnostics do not replace decode error with storage failure");
            }
            state=session.handle("status",{});
            require(rejected && state.at("status")=="planning_failed","repeated failed planning stays fail-closed");
        }
        {
            std::ifstream saved(directory.path+"/setup-draft.json");const auto disk=Json::parse(saved);
            std::size_t used=0;bool omitted=false;
            for(const auto& entry:disk.at("trace")) {
                if(entry.contains("error"))used+=entry.at("error").dump().size();
                if(entry.contains("rejected_raw_hex"))used+=entry.at("rejected_raw_hex").get_ref<const std::string&>().size();
                omitted=omitted||entry.value("diagnostic_budget_exhausted",false);
            }
            require(used<=32768 && omitted,"session-wide diagnostics budget is enforced and omissions labelled");
        }
        session.handle("cancel",{{"session_id",token}});
        require(!session.handle("status",{}).at("active").get<bool>(),"diagnostics never prevent cancel/restore");
        unchanged();state=session.handle("start",{});token=state.at("session_id");
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
