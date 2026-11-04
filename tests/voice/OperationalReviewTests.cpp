// Actual project types/resolver/state store/RuntimeWorker. Only inference and
// STT are explicit doubles; no review-archive scaffolding or hardware access.
#include "Support.h"
#include "firefly/dialogue/OperationalDialogue.h"
#include "firefly/voice/VoiceProcessor.h"
#include <iostream>
using namespace firefly;
using admin_test::Json;
namespace {
int checks = 0, failures = 0;
void expect(bool value, const std::string& label) {
    ++checks;
    if (!value) ++failures;
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
}
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
struct Model : admin_test::Model {
    llm::CommandProtocol commandProtocol() const override { return llm::CommandProtocol::StableCompact; }
};
struct Recognizer : voice::SpeechRecognizer {
    std::string text;
    voice::RecognitionResult recognize(const std::vector<float>&) override {
        voice::RecognitionResult r; r.success = true; r.text = text; return r;
    }
};
Json configuration(const std::string& root) {
    std::ifstream file(root + "/tests/admin/v2/system.json"); auto c = Json::parse(file);
    c["devices"] = Json::array({c["devices"][0]});
    c["devices"][0].update({{"id","lamp"},{"name","Свет"},{"room_id","kitchen"},{"spoken_aliases",{"свет","лампа","лампу"}}});
    c["rooms"] = Json::array({c["rooms"][0]});
    c["rooms"][0].update({{"id","kitchen"},{"name","Кухня"},{"speech",{{"mentions",{"кухня","кухне"}},{"locations",{"на кухне"}}}}});
    c["system"]["voice_dialogue"] = {{"responses","brief"},{"questions_enabled",true}};
    return c;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        auto doc = configuration(argv[1]); auto catalog = admin_test::catalog(doc);
        std::int64_t now = 5000;
        execution::DeviceStateStore states(catalog, [&]{ return now; }, []{ return 200000; });
        states.applyObservation({"lamp",{{"power",actions::Scalar(true)}}});
        dialogue::Facts facts{catalog, 1, now, states.devices(), {}};
        execution::ExecutionResult accepted{true,execution::ExecutionStatus::Accepted,"action_on","lamp",{},{},{},200000};
        facts.history.push_back({10,1,"voice",accepted});
        for (bool enabled : {false, true}) {
            dialogue::OperationalDialogue d;
            auto r = d.ask("А на кухне включи свет", facts, enabled, false);
            expect(!r.handled, std::string("R1 imperative keeps command path, questions=") + (enabled ? "on" : "off"));
        }
        {
            auto aliases = doc; aliases["devices"][0]["spoken_aliases"].push_back("включена");
            aliases["devices"][0]["spoken_aliases"].push_back("почему");
            auto f = facts; f.catalog = admin_test::catalog(aliases);
            dialogue::OperationalDialogue d;
            for (bool enabled : {false,true}) {
                expect(!d.ask("А на кухне включи включена", f, enabled, false).handled, "R1 a state-like device alias is data, not a question marker");
            }
            auto r = d.ask("Состояние почему на кухне", f, true, false);
            expect(r.question == dialogue::Question::State && r.reason == "facts_only", "R1 a question-like device alias cannot choose the question kind");
        }
        {
            dialogue::OperationalDialogue d;
            for (const auto* text : {"Лампа на кухне включена?", "Лампа на кухне выключена?", "Лампа на кухне отключена?"}) {
                auto r = d.ask(text, facts, true, false);
                expect(r.handled && r.question == dialogue::Question::State && (r.text=="Да."||r.text=="Нет."), std::string("R1 feminine state: ") + text);
            }
        }
        {
            dialogue::OperationalDialogue d;
            for (const auto* text : {"Чайник на кухне включен?", "Ты включила чайник на кухне?", "Почему чайник на кухне не работает?", "Свет и чайник на кухне включены?"}) {
                auto r = d.ask(text, facts, true, false);
                expect(r.handled && r.reason != "facts_only" && !has(r.text,"«Свет»"), std::string("R2 unknown device is never replaced: ") + text);
            }
            auto r = d.ask("Что включено на кухне?", facts, true, false);
            expect(r.reason == "facts_only" && has(r.text,"Свет"), "R2 explicit room group still works");
            r = d.ask("А на кухне?", facts, true, false);
            expect(r.reason == "facts_only" && has(r.text,"Свет"), "R2 bare location follow-up still works");
            r = d.ask("Свет на Марсе включен?", facts, true, false);
            expect(r.reason == "unknown_scope", "R2 unknown location remains rejected");
            r = d.ask("Как там чайник на кухне?", facts, true, false, [](auto) { return dialogue::Question::State; });
            expect(r.reason == "unknown_scope", "R2 Qwen code cannot legitimize unknown entity words");
            expect(d.ask("А он сейчас работает?", facts, true, false).reason == "context_missing", "R2 failed scope cannot leave an old device as a pronoun referent");
        }
        {
            dialogue::OperationalDialogue d;
            d.rememberAttempt(facts, {}, "safety");
            auto global = d.ask("Почему команда не выполнилась?", facts, true, false);
            expect(has(global.text,"Не могу выполнить эту команду"), "R3 global last rejection remains visible");
            auto scoped = d.ask("Почему ты не выключила свет на кухне?", facts, true, false);
            expect(has(scoped.text,"Не могу выполнить эту команду") && !has(scoped.text,"Команду отправила"), "R3 unknown rejection scope cannot reveal stale success as last attempt");
            expect(has(scoped.text,"Не знаю, к какому устройству"), "R3 missing target association is explicit uncertainty");
            d.rememberAttempt(facts, {}, "safety", "Не выключай свет на кухне");
            scoped = d.ask("Почему ты не выключила свет на кухне?", facts, true, false);
            expect(has(scoped.text,"Не могу выполнить эту команду") && !has(scoped.text,"Не знаю, к какому устройству"), "R3 exact rejected request retains its independently grounded target");
            for (const auto* text : {"Не выключай чайник на кухне", "Не выключай свет на Марсе", "Да я включил свет"}) {
                d.rememberAttempt(facts, {}, "safety", text);
                scoped = d.ask("Почему ты не выключила свет на кухне?", facts, true, false);
                expect(has(scoped.text,"Не знаю, к какому устройству") && !has(scoped.text,"Команду отправила"), std::string("R3 no guessed rejection target: ") + text);
            }
            facts.history.push_back({11,1,"admin-device",accepted});
            scoped = d.ask("Почему ты не выключила свет на кухне?", facts, true, false);
            expect(has(scoped.text,"Команду отправила"), "R3 genuinely newer device result supersedes older uncertain rejection");
            facts.history.pop_back();
        }
        {
            dialogue::OperationalDialogue d;
            auto last = d.ask("Что ты сделала последним?", facts, true, false);
            expect(has(last.text,"Свет"), "R4 last action actually names its device");
            auto next = d.ask("А он сейчас работает?", facts, true, false);
            expect(next.reason == "facts_only" && next.text=="Да.", "R4 last-action referent survives into state question");
            next = d.ask("А она сейчас включена?", facts, true, false);
            expect(next.reason == "facts_only", "R4 feminine follow-up has same grounded referent");
            facts.steadyMs += 121000;
            expect(d.ask("А он сейчас работает?", facts, true, false).reason == "context_missing", "R4 referent TTL remains enforced");
            facts.steadyMs = now;
            d.ask("Что ты сделала последним?", facts, true, false);
            for (int i = 0; i < 5; ++i) expect(d.ask("А он сейчас работает?", facts, true, false).reason == "facts_only", "R4 bounded read-only follow-up");
            expect(d.ask("А он сейчас работает?", facts, true, false).reason == "context_missing", "R4 six-turn limit remains enforced");
            facts.generation = 2;
            d.ask("Что ты сделала последним?", facts, true, false);
            expect(d.ask("А он сейчас работает?", facts, true, false).reason == "context_missing", "R4 an old generation's last event cannot establish a referent");
            facts.generation = 1;
        }
        {
            auto mixed = doc;
            auto room = mixed["rooms"][0]; room.update({{"id","office"},{"name","Кабинет"},{"speech",{{"mentions",{"кабинет","кабинете"}},{"locations",{"в кабинете"}}}}});
            mixed["rooms"].push_back(room);
            auto fan = mixed["devices"][0]; fan.update({{"id","fan"},{"name","Вентилятор"},{"room_id","office"},{"spoken_aliases",{"вентилятор"}}});
            mixed["devices"].push_back(fan); auto both = admin_test::catalog(mixed);
            execution::DeviceStateStore all(both);
            dialogue::Facts f{both,1,now,all.devices(),facts.history};
            dialogue::OperationalDialogue d;
            auto r = d.ask("Свет в кабинете включен?", f, true, false);
            expect(r.reason == "ambiguous" && !has(r.text,"Вентилятор"), "R2 existing resolver rejects a device/room mismatch");
            d.rememberAttempt(f, {}, "safety", "Не включай вентилятор в кабинете");
            r = d.ask("Почему ты не включила свет на кухне?", f, true, false);
            expect(has(r.text,"Команду отправила") && !has(r.text,"Не могу выполнить эту команду"), "R3 a rejection explicitly about another device is not attributed to this one");
            r = d.ask("Почему ты не включила вентилятор в кабинете?", f, true, false);
            expect(has(r.text,"Не могу выполнить эту команду") && !has(r.text,"Не знаю, к какому устройству"), "R3 second catalog device has its own grounded rejection");
            mixed["devices"][1]["spoken_aliases"].push_back("свет");
            auto sharedAlias = admin_test::catalog(mixed);
            f.catalog = sharedAlias; f.devices = execution::DeviceStateStore(sharedAlias).devices();
            std::vector<std::string> observedTargets;
            r = d.ask("Свет на кухне включен?", f, true, false, {}, [&](const auto& ids){observedTargets=ids;return f;});
            expect(r.reason == "facts_only" && observedTargets==std::vector<std::string>{"lamp"}, "R2 shared aliases in different rooms retain existing exact disambiguation");
        }
        // Full worker reaches existing CommandEngine/SafetyGate/executor facade.
        // Missing binding is intentional: history proves admission without I/O.
        for (bool enabled : {false, true}) {
            auto c = doc; c["system"]["voice_dialogue"]["questions_enabled"] = enabled;
            Model model; model.raw = "[1]"; admin::RuntimeWorker worker(model);
            expect(Json::parse(worker.handle(Json{{"method","apply"},{"configuration",c}}.dump()))["ok"], "R1 worker config admitted");
            const auto baseline = Json::parse(worker.handle(R"({"method":"test","text":"А на кухне включи свет"})"));
            expect(baseline.at("result").value("targetId",std::string{}) == "lamp", "R1 unmodified CommandEngine/SafetyGate accepts the exact original request");
            Recognizer stt; stt.text = "А на кухне включи свет";
            voice::Utterance u; u.audio.resize(16000); u.speech = {0,16000};
            const auto r = worker.processVoice(stt, u);
            expect(r.route != "OPERATIONAL_QUESTION" && r.replyKind == "failed" && r.replyText=="Для этого действия не настроено подключение.", "R1 worker retains actual command admission and missing-binding result");
            expect(Json::parse(worker.observe())["executions"].size() == 1, "R1 exactly one real executor-facade attempt");
            stt.text = "Что ты сделала последним?";
            worker.processVoice(stt, u);
            const auto attempts = Json::parse(worker.observe())["executions"].size();
            stt.text = "А включи его";
            worker.processVoice(stt, u);
            expect(Json::parse(worker.observe())["executions"].size() == attempts, "R4 context cannot fill a command target");
        }
        std::cout << "CHECKS=" << checks << " FAILURES=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << "ERROR " << e.what() << '\n'; return 2; }
}
