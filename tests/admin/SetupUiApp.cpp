#include "Support.h"
#include "HttpServer.h"
#include <csignal>
#include <iostream>
volatile std::sig_atomic_t stopping=0;
void stop(int){stopping=1;}
struct Model:admin_test::Model {
 bool admin=false;
 bool setupRequired() const override{return true;}
 void enterSetup() override{admin=true;}
 void leaveSetup(const std::string& p) override{admin=false;useContext(p);}
 void useAdminContext(const std::string&p,int) override{if(!admin)throw std::runtime_error("setup_required");prompt=p;}
 std::string inferAdmin(std::string_view text) override{
  ++inferences;
  if(prompt.find("Предложи только небольшой русский")!=std::string::npos){
   if(prompt.find("mentions")!=std::string::npos)return R"({"aliases":["детская"],"mentions":["детская","детской"],"locations":["в детской"]})";
   if(prompt.find("assignments")!=std::string::npos)return prompt.find("false")!=std::string::npos?R"({"aliases":["выключи","выключить"]})":R"({"aliases":["включи","включить"]})";
   return R"({"aliases":["устройство"]})";
  }
  if(prompt.find("Извлеки НОВОЕ имя")!=std::string::npos)return R"({"name":"Основной свет"})";
  if(std::string(text).find("ночник")!=std::string::npos)return R"({"ops":[["power_device","Детская","Ночник"]]})";
  if(std::string(text).find("Создай")!=std::string::npos)return R"({"ops":[["location","Детская"]]})";
  return R"({"ops":[["power_device","Детская","Телевизор"],["power_device","Детская","Свет"]]})";
 }
};
struct Recognizer:firefly::voice::SpeechRecognizer {
 firefly::voice::RecognitionResult recognize(const std::vector<float>& samples)override {
  firefly::voice::RecognitionResult r;r.success=!samples.empty();
  r.text="Создай детскую комнату. Там добавь телевизор только с включением и выключением, также добавь свет только с включением и выключением.";
  return r;
 }
};
int main(int argc,char**argv){try{
 if(argc!=3)return 2;
 firefly::config::ConfigStore store(argv[1]);store.initialize(firefly::config::ConfigLoader::fromFile(argv[2]));
 Model model;firefly::admin::RuntimeWorker worker(model);firefly::admin::LocalRuntimeClient client(worker);
 firefly::admin::AdminTranscriber stt(std::make_unique<Recognizer>());
 firefly::admin::Api api(store,client,&stt);api.initializeRuntime();
 firefly::admin::HttpServer server(api,"127.0.0.1",0);
 std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
 std::cout<<"DEMO_LISTEN http://127.0.0.1:"<<server.port()<<std::endl;server.run(stopping);worker.stop();return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
