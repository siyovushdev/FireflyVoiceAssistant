// Real HTTP/API/ConfigStore/runtime/voice path; only STT and NPU are doubles.
#include "../admin/Support.h"
#include "HttpServer.h"
#include <csignal>
#include <iostream>
#include <thread>
using namespace firefly;
using admin_test::Json;
namespace { volatile std::sig_atomic_t stopped=0;
struct Model : admin_test::Model {
    llm::CommandProtocol commandProtocol() const override{return llm::CommandProtocol::StableCompact;}
};
struct Recognizer:voice::SpeechRecognizer {
    std::string text;bool success=true;int calls=0;
    voice::RecognitionResult recognize(const std::vector<float>&)override {++calls;voice::RecognitionResult r;r.success=success;r.text=text;r.error=success?"":"fixture STT error";return r;}
};
}
int main(int argc,char**argv) {
    if(argc!=3)return 2;
    const std::filesystem::path storePath(argv[1]);
    // File-only fixture cannot be accidentally pointed at the real store.
    if(storePath.string().rfind("/tmp/firefly-operational-",0)!=0)return 2;
    try {
        config::ConfigStore store(storePath.string());
        if(!std::filesystem::exists(storePath/"configuration.json"))store.initialize(config::ConfigLoader::fromFile(argv[2]));
        Model model;model.raw="[1]";admin::RuntimeWorker worker(model);admin::LocalRuntimeClient client(worker);admin::Api api(store,client);
        api.initializeRuntime();admin::HttpServer server(api,"127.0.0.1",0);std::thread http([&]{server.run(stopped);});
        std::cout<<Json{{"url","http://127.0.0.1:"+std::to_string(server.port())},{"pid",getpid()}}.dump()<<'\n'<<std::flush;
        try {
            std::string line;Recognizer rec;voice::Utterance u;u.audio.resize(16000);u.speech={0,16000};
            while(std::getline(std::cin,line)) {
                auto input=Json::parse(line);const auto method=input.at("method").get<std::string>();Json result;
                if(method=="voice") {
                    rec.text=input.at("text");rec.success=input.value("stt_success",true);u.inputGeneration=input.value("input_generation",std::uint64_t(0));auto r=worker.processVoice(rec,u);
                    result={{"text",r.commandText},{"reply",r.replyText},{"kind",r.replyKind},{"reason",r.replyReason},{"mode",r.replyMode},{"route",r.route},{"llm_invoked",r.llmTimings.invoked},{"error",r.error},{"stt_calls",rec.calls}};
                }else if(method=="input-defaults") {worker.setWakeInputDefaults(input.at("after_beep"),input.at("confirmation"));result={{"ok",true}};}else if(method=="runtime")result=Json::parse(worker.handle(input.at("request").dump()));
                else if(method=="model") {model.available=input.at("available");result={{"ok",true}};}
                else if(method=="quit")break;
                else throw std::runtime_error("Unknown fixture method");
                const auto policy=worker.wakeInputPolicy();result["input_policy"]={{"after_beep",policy.afterBeepOnly},{"generation_id",policy.generation}};result["observation"]=Json::parse(worker.observe());std::cout<<result.dump()<<'\n'<<std::flush;
            }
        }catch(...){stopped=1;worker.stop();http.join();server.finishClients();throw;}
        stopped=1;api.beginShutdown();worker.stop();http.join();server.finishClients();
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
