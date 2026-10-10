// On-board acceptance only; never scheduled by CTest with another NPU owner.
// Text is injected at the recognizer seam; this is not a microphone/STT test.
#include "RuntimeModel.h"
#include "SetupSession.h"
#include "firefly/admin/RuntimeWorker.h"
#include <filesystem>
#include <csignal>
#include <iostream>
using namespace firefly;
using admin::Json;
namespace {
volatile std::sig_atomic_t cancelled=0;
void cancel(int){cancelled=1;}
void require(bool good,const std::string& why){if(!good)throw std::runtime_error(why);}
class CountingModel final:public admin::ContextModel {
    admin::RuntimeModel model_{
        "/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm",
        "/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm"};
public:
    int runtimeCalls=0,adminCalls=0;
    llm::CommandProtocol commandProtocol()const override{return model_.commandProtocol();}
    void useContext(const std::string& p)override{model_.useContext(p);}
    void useAdminContext(const std::string& p,int budget)override{model_.useAdminContext(p,budget);}
    bool ready()const override{return model_.ready();}
    int contextTokens()const override{return model_.contextTokens();}
    bool setupRequired()const override{return true;}
    void enterSetup()override{model_.enterSetup();}
    void leaveSetup(const std::string& p)override{model_.leaveSetup(p);}
    std::string infer(std::string_view text)override{++runtimeCalls;return model_.infer(text);}
    std::string inferCommand(std::string_view text,std::string_view contract)override{++runtimeCalls;return model_.inferCommand(text,contract);}
    std::string inferAdmin(std::string_view text)override{++adminCalls;return model_.inferAdmin(text);}
    std::string inferenceMetrics()const override{return model_.inferenceMetrics();}
};
struct Transcript final:voice::SpeechRecognizer {
    std::string text;
    voice::RecognitionResult recognize(const std::vector<float>&)override{return {true,text,"",0};}
};
}
int main(int argc,char**argv){
    if(argc!=3 && argc!=4){std::cerr<<"OUTPUT (--create | --config FILE | --restart STORE)\n";return 2;}
    ::umask(0077);std::signal(SIGTERM,cancel);std::signal(SIGINT,cancel);
    Json report={{"complete",false},{"rows",Json::array()},{"language_proposals",Json::array()},
        {"measurement","RuntimeWorker::processVoice with injected transcript, real models, no bindings"}};
    const auto save=[&]{std::ofstream(argv[1])<<report.dump(2)<<'\n';};
    try {
        for(const auto& p:std::filesystem::directory_iterator("/proc")) {
            const auto pid=p.path().filename().string();
            if(pid.empty() || pid.find_first_not_of("0123456789")!=std::string::npos || pid==std::to_string(::getpid()))continue;
            std::ifstream maps(p.path()/"maps");std::string line;
            while(std::getline(maps,line))require(line.find("librkllmrt.so")==std::string::npos,"Another RKLLM owner: "+p.path().filename().string());
        }
        const std::string mode=argv[2];std::unique_ptr<config::ConfigStore> store;config::Catalog catalog;
        if(mode=="--create" && argc==3) {
            auto pattern=(std::filesystem::absolute(std::filesystem::path(argv[1]).parent_path())/"store-XXXXXX").string();
            const auto dir=::mkdtemp(pattern.data());require(dir,"Cannot create private test store");
            store=std::make_unique<config::ConfigStore>(dir);report["test_store"]=dir;
            catalog=store->initialize(config::ConfigLoader::fromFile("config/empty-home.json"));
        } else if(mode=="--restart" && argc==4) {
            store=std::make_unique<config::ConfigStore>(argv[3]);catalog=store->current();report["test_store"]=argv[3];
        } else if(mode=="--config" && argc==4) catalog=config::ConfigLoader::fromFile(argv[3]);
        else throw std::runtime_error("Invalid arguments");
        require(catalog->configuration().bindings.empty(),"Acceptance refuses a catalog with hardware bindings");
        CountingModel model;admin::RuntimeWorker worker(model);
        const auto call=[&](const Json& request){
            require(!cancelled,"Cancelled before model request");
            std::cout<<"CALL "<<request.at("method")<<std::endl;
            auto reply=Json::parse(worker.handle(request.dump()));
            if(request.at("method")=="language_profile") {
                report["language_proposals"].push_back({{"entity",request.at("entity")},{"id",request.at("id")},{"reply",reply}});save();
            }
            require(!cancelled,"Cancelled after model request");return reply;
        };
        require(call({{"method","apply"},{"configuration",Json::parse(catalog->canonicalJson())}}).value("ok",false),"Initial runtime apply");
        if(mode=="--create") {
            admin::SetupSession setup(*store,call);auto state=setup.handle("start",{});const auto token=state.at("session_id");
            int number=0;
            for(const auto& text:{"Создай детскую комнату. Там добавь телевизор и свет только с включением и выключением.",
                "Создай бирюзовую комнату. Там добавь вентилятор только с включением и выключением."}) {
                const auto begin=std::chrono::steady_clock::now();const auto before=model.adminCalls;
                state=setup.handle("message",{{"session_id",token},{"version",state.at("version")},{"request_id",std::to_string(++number)},{"text",text}});
                report["messages"].push_back({{"text",text},{"state",state},{"admin_calls",model.adminCalls-before},
                    {"wall_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()}});save();
                require(state.at("status")=="editing","Dialogue did not finish");
            }
            const auto& graph=state.at("configuration");
            require(graph.at("rooms").size()==2&&graph.at("devices").size()==3,"Unexpected graph");
            require(graph.at("bindings").empty(),"AI invented bindings");
            for(const auto& d:graph.at("devices"))require(d.at("action_ids").size()==2&&d.at("state").size()==1,"Unexpected capabilities");
            std::map<std::string,int> accepted;
            for(const auto& p:report["language_proposals"])if(p.at("reply").value("ok",false))++accepted[p.at("entity").get<std::string>()];
            require(accepted["location"]>=2&&accepted["device"]>=3&&accepted["action"]>=6,"Language profiles not accepted for all created entities");
            report["apply"]=setup.handle("apply",{{"session_id",token},{"version",state.at("version")}});save();
            require(report["apply"].at("status")=="applied","Apply did not restore runtime");catalog=store->current();
            const auto persisted=Json::parse(catalog->canonicalJson());
            for(const auto& proposal:report["language_proposals"]) if(proposal.at("reply").value("ok",false)) {
                const auto entity=proposal.at("entity").get<std::string>();
                const auto collection=entity=="location"?"rooms":entity=="device"?"devices":"actions";
                const auto target=std::find_if(persisted.at(collection).begin(),persisted.at(collection).end(),[&](const auto& item){return item.at("id")==proposal.at("id");});
                require(target!=persisted.at(collection).end(),"Proposed entity not saved");
                for(const auto& field:proposal.at("reply").at("suggestion").items()) if(field.key()!="conflicts") {
                    const auto& atoms=entity=="location"?(field.key()=="aliases"?target->at("metadata").at("aliases"):target->at("speech").at(field.key())):
                        entity=="device"?target->at("spoken_aliases"):target->at("speech_aliases");
                    for(const auto& atom:field.value())require(std::find(atoms.begin(),atoms.end(),atom)!=atoms.end(),"Decoded language atom did not survive Save/Apply");
                }
            }
            report["persisted_language_profiles_verified"]=true;save();
        }
        report["hash"]=catalog->contentHash();report["revision"]=catalog->configuration().revision;
        require(catalog->configuration().bindings.empty(),"No hardware bindings allowed");
        const std::vector<std::string> corpus={"Включи свет в детской","Включи свет в детской комнате","В детской включи свет",
            "Свет в детской включи","Пожалуйста включи свет в детской","Включи свет на детском","Выключи свет в детской",
            "Включи телевизор в детской","Выключи телевизор в детской"};
        std::vector<std::string> inputs=corpus;
        if(mode!="--config")for(const auto* text:{"Включи вентилятор в бирюзовой комнате","В бирюзовой выключи вентилятор","Вентилятор на бирюзовом включи"})inputs.push_back(text);
        Transcript recognizer;voice::Utterance utterance{{0,16000},std::vector<float>(16000),"",false};
        std::vector<double> latencies;int successful=0;
        const auto checkCommand=[&](const std::string& text,bool positive,bool record){
            require(!cancelled,"Cancelled before replay");recognizer.text=text;const auto before=model.runtimeCalls;
            const auto result=worker.processVoice(recognizer,utterance);const auto observed=Json::parse(worker.observe());
            const auto& command=observed.at("last_voice").at("command");require(!command.is_null(),"Voice pipeline did not call CommandEngine");
            const bool accepted=std::holds_alternative<actions::GenericAction>(result.action);
            if(record){auto row=command;row["qwen_calls"]=model.runtimeCalls-before;row["expected_positive"]=positive;report["rows"].push_back(row);save();}
            require(accepted==positive,"Unexpected interpretation: "+text);
            require(model.runtimeCalls==before,"Runtime Qwen called: "+text);
            require(command.at("hash")==report["hash"] && command.at("language_index").at("hash")==report["hash"] && command.at("language_index").at("revision")==report["revision"],"Mixed snapshots");
            if(positive) {
                require(command.at("language_route")=="INDEX_EXACT"&&command.at("safety")=="PASS"&&command.at("domain")=="PASS","Not a successful index hit");
                require(command.at("execution").at("error")=="binding_missing","Physical execution was not safely blocked");
                const auto& action=std::get<actions::GenericAction>(result.action);
                const auto& target=catalog->configuration().devices.at(catalog->deviceIndex().at(action.targetId()));
                const auto& location=catalog->configuration().rooms.at(catalog->roomIndex().at(*target.roomId));
                const auto& definition=catalog->configuration().actions.at(catalog->actionIndex().at(action.actionId()));
                const bool off=text.find("Выключи")!=std::string::npos||text.find("выключи")!=std::string::npos;
                require(definition.parameters.empty()&&definition.assignments.size()==1&&definition.assignments[0].value==actions::Scalar(!off),"Wrong ON/OFF");
                const auto noun=text.find("телевизор")!=std::string::npos?"телевизор":text.find("вентилятор")!=std::string::npos||text.find("Вентилятор")!=std::string::npos?"вентилятор":"свет";
                require(config::normalizeLanguageAtom(target.name).find(noun)!=std::string::npos,"Wrong selected device");
                require(config::normalizeLanguageAtom(location.name).find(std::string(noun)=="вентилятор"?"бирюзов":"детск")!=std::string::npos,"Wrong selected location");
                if(record)++successful;latencies.push_back(command.at("latency_ms").get<double>());
            }
        };
        for(const auto& text:inputs)checkCommand(text,true,true);
        for(const auto* text:{"Не включи свет в детской","Включи свет в детской на 40 процентов","Включи свет в детской и телевизор",
            "Включи свет на детском столе","Если темно включи свет в детской","Включи свет в детской через час"})checkCommand(text,false,true);
        // Measure understanding only; no STT, VAD wait, initialization or model
        // bootstrap in latency_ms. Refused binding resolution has no transport.
        for(int repeat=0;repeat<20;++repeat)for(const auto& text:inputs)checkCommand(text,true,false);
        std::sort(latencies.begin(),latencies.end());
        report["successful_without_runtime_qwen"]=successful;report["positive_count"]=inputs.size();report["rejected_count"]=6;
        report["runtime_model_calls"]=model.runtimeCalls;report["admin_model_calls"]=model.adminCalls;
        report["text_parse_median_ms"]=latencies[latencies.size()/2];report["text_parse_p95_ms"]=latencies[latencies.size()*95/100];
        report["complete"]=true;save();std::cout<<"PASS positive="<<successful<<" runtime_model_calls="<<model.runtimeCalls<<std::endl;
    }catch(const std::exception& e){report["error"]=e.what();save();std::cerr<<e.what()<<'\n';return 1;}
}
