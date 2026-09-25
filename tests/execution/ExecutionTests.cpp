#include "../admin/Support.h"
#include "firefly/execution/ActionExecutor.h"
#include <future>
#include <iostream>
#include <chrono>
using namespace firefly;
using namespace admin_test;
namespace {
std::atomic<int> checks{0};
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
actions::GenericAction action(std::string target, std::int64_t value, std::string id = "action_set_brightness") {
    return {std::move(id), std::move(target), {{"brightness", actions::IntegerArgument(value)}}};
}
std::int64_t brightness(const std::vector<execution::DeviceRecord>& states, std::string id = "device_kitchen_light") {
    for (const auto& device : states) if (device.targetId == id) return std::get<execution::LightState>(device.state).brightness;
    throw std::runtime_error("missing test device");
}
Json call(admin::Api& api, std::string method, std::string path, Json body = nullptr, int expectedStatus = 200) {
    auto response = api.handle(method, path, body.is_null() ? "" : body.dump());
    if (response.status != expectedStatus) throw std::runtime_error(path + ": " + response.body);
    ++checks; return Json::parse(response.body);
}
Json expected(const Json& response) { return {{"revision",response["revision"]},{"hash",response["hash"]}}; }
class BlockingRecognizer : public voice::SpeechRecognizer {
public:
    std::promise<void> entered, release;
    voice::RecognitionResult recognize(const std::vector<float>&) override {
        entered.set_value(); release.get_future().wait();
        return {true,"В ванне поставь свет на 30 процентов","",0};
    }
};
class Recognizer : public voice::SpeechRecognizer {
public:
    std::string text = "В ванне поставь свет на 30 процентов";
    voice::RecognitionResult recognize(const std::vector<float>&) override { return {true,text,"",0}; }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto config = base(argv[1]);
        const auto catalogA = catalog(config);
        execution::MockDeviceExecutor executor(catalogA);
        check(executor.states().size() == 5, "initial registry");
        for (auto target : {"device_kitchen_light","device_bedroom_light","device_living_light","device_corridor_light"}) {
            for (std::int64_t value : {0,40,100}) {
                const auto result = executor.execute(action(target,value));
                check(result.success && brightness(executor.states(),target) == value, "light execution");
            }
        }
        for (const auto& invalid : std::vector<actions::GenericAction>{action("unknown",40),action("device_kitchen_light",-1),
                action("device_kitchen_light",101),action("device_kitchen_light",40,"unknown"),
                {"action_set_brightness","device_kitchen_light",{}},
                {"action_set_brightness","device_kitchen_light",{{"wrong",actions::IntegerArgument(std::int64_t(40))}}},
                action("device_entrance_door",40), action("device_entrance_door",40,"action_open_entrance"),
                {"action_open_entrance","device_kitchen_light",{}}}) {
            const auto before = executor.states();
            check(!executor.execute(invalid).success, "invalid action rejected");
            for (std::size_t i=0;i<before.size();++i) check(before[i].state == executor.states()[i].state, "reject unchanged");
        }
        const actions::GenericAction door("action_open_entrance","device_entrance_door",{});
        auto first = executor.execute(door), second = executor.execute(door);
        check(first.success && second.success && second.previous == second.current, "idempotent door open");
        auto initialNarrow=config; initialNarrow["actions"][0]["parameters"][0]["minimum"]=10;
        execution::MockDeviceExecutor narrowInitial(catalog(initialNarrow));
        check(brightness(narrowInitial.states())==10,"new light initial minimum valid");
        Model model;
        admin::RuntimeGenerations generations(model);
        generations.apply(catalogA);
        auto one = generations.current();
        auto preview = generations.process("На кухне поставь свет на 40 процентов");
        check(!preview.execution && brightness(one->executor->states()) == 0, "preview unchanged");
        check(generations.process("На кухне поставь свет на 40 процентов",execution::ExecutionIntent::Playground).execution->success, "text executes");
        for (const auto& text : {"Какая сегодня погода","Открой дверь","На кухне выключи свет на 50 процентов"}) {
            model.raw = light("device_kitchen_light",50).dump();
            auto result = generations.process(text,execution::ExecutionIntent::Playground);
            check(!result.execution && std::holds_alternative<actions::Unsupported>(result.command.action), "unsafe not executed");
            check(brightness(one->executor->states()) == 40, "unsafe unchanged");
        }
        auto b = bathroom(config); b["revision"] = 2;
        generations.apply(catalog(b)); auto two = generations.current();
        check(two->id == 2 && two != one, "generation swap");
        check(brightness(two->executor->states()) == 40 && brightness(two->executor->states(),"device_bathroom_light") == 0, "reconcile preserve/new");
        check(two->prompt.find("device_bathroom_light") != std::string::npos, "new LLM context");
        check(generations.process("В ванной поставь свет на 40 процентов",execution::ExecutionIntent::Playground).execution->targetId == "device_bathroom_light", "new entity execution");
        auto narrowed=b; narrowed["actions"][0]["parameters"][0]["maximum"]=30;
        bool rejected=false; try {generations.apply(catalog(narrowed));} catch (const config::ConfigError& e) {rejected=e.code=="state_conflict";}
        check(rejected && generations.current()==two && model.prompt==two->prompt,"range conflict before context swap");
        auto c=config; c["revision"]=3;
        generations.apply(catalog(c));
        check(!generations.current()->executor->execute(action("device_bathroom_light",30)).success,"removed target no fallback");
        check(two->executor->states().size()==6,"retained generation lifetime");
        // Request A pins N from admission, even before recognition. Apply waits;
        // state is copied only after A executed, never against registry N+1.
        auto request=generations.beginRequest(); const auto old=request->generation();
        std::promise<void> started;
        auto apply=std::async(std::launch::async,[&]{started.set_value();generations.apply(catalog(b));});
        started.get_future().wait();
        check(apply.wait_for(std::chrono::milliseconds(40))==std::future_status::timeout,"apply waits request lease");
        auto result=request->process("На кухне поставь свет на 70 процентов",execution::ExecutionIntent::Playground,"voice");
        check(result.generation==old && result.execution->success,"request A executes N");
        request.reset(); apply.get();
        check(generations.current()!=old && brightness(generations.current()->executor->states())==70,"N+1 inherits completed A state");
        check(generations.process("В ванне поставь свет на 30 процентов",execution::ExecutionIntent::Playground).generation==generations.current(),"request B uses N+1");
        for (int i=0;i<105;++i) generations.process("На кухне поставь свет на 40 процентов",execution::ExecutionIntent::Playground);
        check(generations.history().size()==100 && generations.history().back().sequence>100,"bounded history");
        generations.stop(); rejected=false; try {generations.process("На кухне выключи свет",execution::ExecutionIntent::Playground);} catch (...) {rejected=true;}
        check(rejected,"shutdown rejects admission");
        // Real Api/Store/Worker/VoiceProcessor, only inference and STT mocked.
        Model stoppedModel; admin::RuntimeGenerations stoppingRuntime(stoppedModel); stoppingRuntime.apply(catalogA);
        auto stoppingRequest=stoppingRuntime.beginRequest(); stoppingRuntime.stop();
        auto cancelled=stoppingRequest->process("На кухне поставь свет на 40 процентов",execution::ExecutionIntent::Playground);
        check(!cancelled.execution && brightness(cancelled.generation->executor->states())==0,"shutdown suppresses admitted execution");
        Directory directory; config::ConfigStore store(directory.path); store.initialize(catalogA);
        Model apiModel; admin::RuntimeWorker worker(apiModel); admin::LocalRuntimeClient client(worker); admin::Api api(store,client);
        api.initializeRuntime();
        auto stored=call(api,"GET","/api/config");
        call(api,"POST","/api/config/validate",{{"configuration",b}});
        auto saved=call(api,"PUT","/api/config",{{"configuration",b},{"expected",expected(stored)}});
        call(api,"POST","/api/config/apply",{{"expected",expected(saved)}});
        auto active=call(api,"GET","/api/config/active");
        check(active["active"]["context"].get<std::string>().find("device_bathroom_light")!=std::string::npos,"API applied context");
        auto test=call(api,"POST","/api/test-command",{{"text","В ванной поставь свет на 40 процентов"}});
        check(test["execution"].is_null(),"API preview");
        test=call(api,"POST","/api/test-command",{{"text","В ванной поставь свет на 40 процентов"},{"execute",true}});
        check(test["execution"]["current"]["brightness"]==40,"API text execute40");
        Recognizer recognizer;
        voice::Utterance utterance; utterance.audio.resize(16000); utterance.speech={0,16000};
        auto spoken=worker.processVoice(recognizer,utterance);
        check(std::get<actions::GenericAction>(spoken.action).targetId()=="device_bathroom_light","voice after Apply same runtime");
        auto state=call(api,"GET","/api/devices/state");
        bool productionUnknown=false; for (const auto& device:state["devices"]) if(device["targetId"]=="device_bathroom_light")
            productionUnknown = device["properties"]["brightness"]["knowledge"]=="unknown";
        check(productionUnknown,"playground execute does not mutate production DeviceStateStore");
        auto history=call(api,"GET","/api/executions");
        check(history["executions"].size()==2 && history["executions"].back()["source"]=="voice","history sources no preview");
        check(history["executions"].back()["execution"]["success"]==false &&
            history["executions"].back()["execution"]["error"]=="binding_missing","production voice fail-closed without binding");
        call(api,"POST","/api/test-command",{{"text",std::string(300000,'x')},{"execute",true}},422);
        check(call(api,"GET","/api/status")["last_command"]["input"]=="[oversized rejected input omitted]","bounded last diagnostics");
        worker.processVoice(recognizer,utterance);
        auto status=call(api,"GET","/api/status");
        check(status["active"]["generation_id"]==2 && status["device_count"]==6 && status["last_command"]["execution"]["success"]==false,"live status after voice binding_missing");
        // Playground execute still mutates virtual simulation when bindings are absent.
        test=call(api,"POST","/api/test-command",{{"text","В ванной поставь свет на 30 процентов"},{"execute",true}});
        check(test["execution"]["success"]==true && test["execution"]["current"]["brightness"]==30,"playground mock execute without bindings");
        state=call(api,"GET","/api/devices/state");
        productionUnknown=false; for (const auto& device:state["devices"]) if(device["targetId"]=="device_bathroom_light")
            productionUnknown = device["properties"]["brightness"]["knowledge"]=="unknown";
        check(productionUnknown,"second playground execute still leaves production Unknown");
        call(api,"POST","/api/test-command",{{"text","Открой входную дверь"},{"execute","yes"}},400);
        // Actual VoiceProcessor STT lifecycle: Apply is requested while recognition
        // is in flight. Read-only status/state must remain available meanwhile.
        BlockingRecognizer blocking;
        auto speaking=std::async(std::launch::async,[&]{return worker.processVoice(blocking,utterance);});
        blocking.entered.get_future().wait();
        auto live=call(api,"GET","/api/status");
        check(live["runtime_busy"]==true,"status responsive during STT");
        auto d=b; d["devices"][5]["name"]="Bathroom renamed";
        auto savedAgain=call(api,"PUT","/api/config",{{"configuration",d},{"expected",expected(saved)}});
        auto applyingVoice=std::async(std::launch::async,[&]{return call(api,"POST","/api/config/apply",{{"expected",expected(savedAgain)}});});
        check(applyingVoice.wait_for(std::chrono::milliseconds(40))==std::future_status::timeout,"Apply waits admitted voice STT");
        check(call(api,"GET","/api/devices/state")["active"]["generation_id"]==2,"read states during pending Apply");
        blocking.release.set_value();
        check(std::holds_alternative<actions::GenericAction>(speaking.get().action),"voice completes old generation");
        check(applyingVoice.get()["active"]["generation_id"]==3,"next voice generation published");
        check(call(api,"GET","/api/executions")["executions"].back()["generation_id"]==2,"voice execution attributed to old generation");
        auto nextVoice=worker.processVoice(recognizer,utterance);
        check(std::holds_alternative<actions::GenericAction>(nextVoice.action) &&
            call(api,"GET","/api/executions")["executions"].back()["generation_id"]==3,"next voice uses applied generation");
        saved=savedAgain;
        apiModel.reject=true;
        auto failed=call(api,"POST","/api/config/apply",{{"expected",expected(saved)}},422);
        check(failed["active"]["generation_id"]==3,"failed apply retains generation");
        std::cout << "PASS " << checks.load() << " executor/unified/API/voice checks\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
