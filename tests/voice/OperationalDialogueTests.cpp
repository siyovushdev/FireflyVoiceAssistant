#include "Support.h"
#include "firefly/dialogue/OperationalDialogue.h"
#include "firefly/voice/VoiceProcessor.h"
#include <iostream>
#include <future>
using namespace firefly;
using admin_test::Json;
namespace {
int checks=0;
void check(bool value,const std::string& name){++checks;if(!value)throw std::runtime_error(name);}
bool contains(const std::string& text,const std::string& needle){return text.find(needle)!=std::string::npos;}
Json fixture(const std::string& root) {
    std::ifstream file(root+"/tests/admin/v2/system.json");auto doc=Json::parse(file);
    auto d=doc["devices"][0];d["id"]="kitchen_light";d["name"]="Свет";d["spoken_aliases"]={"свет"};d["room_id"]="kitchen";doc["devices"]=Json::array({d});
    auto room=doc["rooms"][0];room["id"]="kitchen";room["name"]="Кухня";room["speech"]={{"mentions",{"кухня","кухне"}},{"locations",{"на кухне"}}};doc["rooms"]=Json::array({room});
    doc["connections"]=Json::array();doc["bindings"]=Json::array();doc["observations"]=Json::array();
    return doc;
}
struct Model:admin_test::Model {
    std::string inferCommand(std::string_view text,std::string_view contract)override {
        // Match the real RKLLM envelope: schema must be bounded JSON, never
        // prose copied out of the system prompt. This is not an inference oracle.
        if(contract.empty()||contract.size()>1024)throw std::invalid_argument("Invalid command envelope");
        const auto schema=Json::parse(contract);
        if(!schema.is_object())throw std::invalid_argument("Invalid schema");
        return infer(text);
    }
};
struct Recognizer:voice::SpeechRecognizer {
    std::string text;
    voice::RecognitionResult recognize(const std::vector<float>&)override {voice::RecognitionResult r;r.success=true;r.text=text;return r;}
};
}
int main(int argc,char**argv) {
    try {
        if(argc!=2)return 2;
        auto doc=fixture(argv[1]);auto legacy=admin_test::catalog(doc);
        check(!contains(legacy->canonicalJson(),"voice_dialogue"),"old canonical bytes omit defaults");
        doc["system"]["voice_dialogue"]={{"responses","brief"},{"questions_enabled",true}};
        auto c=admin_test::catalog(doc);check(c->configuration().system.voiceDialogue.responses=="brief","settings decode");
        check(admin_test::catalog(Json::parse(c->canonicalJson()))->contentHash()==c->contentHash(),"settings canonical roundtrip");
        for(const Json bad:{Json{{"responses","verbose"},{"questions_enabled",true}},Json{{"responses","brief"},{"questions_enabled",1}},Json{{"responses","brief"},{"questions_enabled",true},{"model","other"}}}) {
            auto b=doc;b["system"]["voice_dialogue"]=bad;try{admin_test::catalog(b);check(false,"bad setting accepted");}catch(const config::ConfigError&){check(true,"bad setting rejected");}
        }
        std::int64_t now=5000;
        execution::DeviceStateStore states(c,[&]{return now;},[]{return 200000;});
        dialogue::Facts f{c,1,now,states.devices(),{}};
        dialogue::OperationalDialogue dialog;
        auto answer=dialog.ask("Свет на кухне включен?",f,true,false);
        check(answer.handled&&answer.text=="Сейчас не могу проверить.","initial=true config seed is not physical state");
        states.applyPrediction("kitchen_light",{{"power",actions::Scalar(true)}},200000);f.devices=states.devices();
        answer=dialog.ask("Что сейчас включено?",f,true,false);
        check(contains(answer.text,"не могу проверить")&&!contains(answer.text,"Включены:"),"predicted ON not confirmed ON");
        answer=dialog.ask("А он сейчас работает?",f,true,false);check(contains(answer.text,"не могу проверить"),"single reported device is a valid read-only referent");
        dialog.ask("Что сейчас включено?",f,true,false);
        answer=dialog.ask("А на кухне?",f,true,false);check(contains(answer.text,"не могу проверить"),"location followup retains read-only question kind");
        answer=dialog.ask("А он сейчас работает?",f,true,false);check(contains(answer.text,"не могу проверить"),"pronoun state from one known target");
        f.steadyMs=now+121000;answer=dialog.ask("А он сейчас работает?",f,true,false);check(answer.reason=="context_missing","context expires");
        f.steadyMs=now;
        states.applyObservation({"kitchen_light",{{"power",actions::Scalar(true)}}});f.devices=states.devices();
        answer=dialog.ask("Что сейчас включено?",f,true,false);check(contains(answer.text,"Включены: Свет"),"fresh confirmed ON list");
        f.steadyMs=now+60001;answer=dialog.ask("Что сейчас включено?",f,true,false);check(!contains(answer.text,"Включены:"),"stale confirmed excluded from current on list");f.steadyMs=now;
        execution::ExecutionResult event{true,execution::ExecutionStatus::Accepted,"action_on","kitchen_light",{},{},{},200000};
        f.history.push_back({1,1,"playground",event});
        answer=dialog.ask("Что ты сделала последним?",f,true,false);check(contains(answer.text,"ещё не отправляла"),"mock history is not actual execution");
        f.history.push_back({2,1,"voice",event});
        answer=dialog.ask("Ты включила свет?",f,true,false);check(answer.text=="Питание включено.","action receipt and observed facts are separate");
        event.success=false;event.status=execution::ExecutionStatus::Failed;event.error="mqtt_publish_timeout";
        dialog.rememberAttempt(f,event,{});answer=dialog.ask("Почему команда не выполнилась?",f,true,false);check(answer.text=="Не удалось подтвердить выполнение.","timeout never falsely promises no effect");
        dialog.rememberAttempt(f,{},"safety");answer=dialog.ask("Почему команда не выполнилась?",f,true,false);check(answer.text=="Не могу выполнить эту команду.","guard rejection correctly means no dispatch");
        answer=dialog.ask("Что включено на марсе?",f,true,false);check(answer.reason=="unknown_scope","unknown location cannot silently become global");
        answer=dialog.ask("Свет на марсе включен?",f,true,false);check(answer.reason=="unknown_scope","known device does not erase an unknown location");
        answer=dialog.ask("Ты включила чайник?",f,true,false);check(answer.reason=="unknown_scope","unknown device cannot silently become last lamp");
        answer=dialog.ask("Сколько лет луне?",f,true,false,[](auto){return dialogue::Question::None;});check(answer.reason=="outside_operational_scope","outside topics unsupported");
        answer=dialog.ask("Что сейчас включено?",f,false,false,[](auto){throw std::runtime_error("disabled questions must not invoke Qwen");return dialogue::Question::State;});check(answer.reason=="questions_disabled","questions switch");
        for(const auto* phrase:{"да будет свет на кухне","а отключи ка свет на кухне","можешь включить свет пожалуйста","не выключай свет","да я включил свет"})check(!dialogue::OperationalDialogue::questionLike(phrase),std::string("existing command/safety path preserved: ")+phrase);
        Model model;admin::RuntimeWorker worker(model);
        auto applied=Json::parse(worker.handle(Json{{"method","apply"},{"configuration",doc}}.dump()));check(applied["ok"]==true,"worker generation starts");
        Recognizer rec;voice::Utterance u;u.audio.resize(16000);u.speech={0,16000};
        for(const auto* raw:{"[1]","[999]","{\"actionId\":\"action_on\",\"targetId\":\"kitchen_light\",\"arguments\":{}}"}) {
            model.raw=raw;
            for(const auto* text:{"Ты включила свет?","Что сейчас включено?","Как прошёл запуск?","А он сейчас работает?","Почему команда не выполнилась?"}) {
                rec.text=text;auto result=worker.processVoice(rec,u);
                check(result.route=="OPERATIONAL_QUESTION"&&std::holds_alternative<actions::Unsupported>(result.action),"questions do not become executable even with forged Qwen output");
                check(Json::parse(worker.observe())["executions"].empty(),"questions never execute");
                check(!result.replyText.empty()&&result.replyMode=="brief","worker exposes grounded reply");
            }
        }
        model.raw="[3]";rec.text="Как прошёл запуск?";auto classified=worker.processVoice(rec,u);
        check(classified.llmTimings.invoked&&contains(classified.replyText,"ещё не отправляла"),"bounded JSON classifier contract reaches inference and returns a read-only intent");
        rec.text="включи свет на кухне";auto command=worker.processVoice(rec,u);
        check(command.replyKind=="failed"&&command.replyText=="Для этого действия не настроено подключение.","actual executor missing binding produces honest failure");
        check(Json::parse(worker.observe())["executions"].size()==1,"positive control reaches real executor facade");
        auto p=doc;p["system"]["voice_dialogue"]["responses"]="off";worker.handle(Json{{"method","apply"},{"configuration",p}}.dump());
        rec.text="Что сейчас включено?";auto muted=worker.processVoice(rec,u);check(muted.replyMode=="off"&&muted.route=="OPERATIONAL_QUESTION","live Apply mutes without restarting worker");
        auto epoch=worker.speechEpoch();check(Json::parse(worker.handle("{\"method\":\"cancel_speech\"}"))["ok"]==true&&worker.speechEpoch()>epoch,"stop speech independent cancellation epoch");
        {
            admin_test::Directory directory;config::ConfigStore store(directory.path);store.initialize(admin_test::catalog(doc));
            admin::LocalRuntimeClient local(worker);admin::Api api(store,local);
            model.block=true;model.entered=false;model.release=false;model.raw="[1]";
            auto inference=std::async(std::launch::async,[&]{return api.handle("POST","/api/test-command",R"({"text":"да будет свет на кухне"})");});
            {std::unique_lock<std::mutex> lock(model.mutex);model.cv.wait_for(lock,std::chrono::seconds(3),[&]{return model.entered;});}
            const auto started=std::chrono::steady_clock::now();auto stopped=api.handle("POST","/api/voice/stop","{}");
            const auto elapsed=std::chrono::steady_clock::now()-started;
            {std::lock_guard<std::mutex> lock(model.mutex);model.release=true;}model.cv.notify_all();inference.get();model.block=false;
            check(stopped.status==200&&elapsed<std::chrono::milliseconds(100),"audio stop bypasses a busy API/model operation");
        }
        std::cout<<"PASS "<<checks<<" operational dialogue/schema/runtime checks; no NPU/mic/network\n";
    }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
