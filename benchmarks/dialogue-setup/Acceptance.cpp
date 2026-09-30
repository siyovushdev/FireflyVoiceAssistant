#include "RuntimeModel.h"
#include "SetupSession.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/admin/RuntimeWorker.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/PcmAudio.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <csignal>
using namespace firefly::admin;
volatile std::sig_atomic_t cancelled=0;
void cancel(int){cancelled=1;}
int main(int argc,char**argv){
 if(argc!=2&&argc!=3)return 2;
 std::signal(SIGTERM,cancel);std::signal(SIGINT,cancel);
 std::ofstream("/proc/self/oom_score_adj")<<1000;
 Json report={{"cases",Json::array()}};
 const std::string output=argv[1];
 const auto save=[&]{std::ofstream(output)<<report.dump(2)<<'\n';};
 try{
  std::string pattern=std::filesystem::absolute("build/dialogue-recovery/store-XXXXXX").string();
  auto dir=::mkdtemp(pattern.data());if(!dir)throw std::runtime_error("test store unavailable");
  report["test_store"]=dir;save();
  firefly::config::ConfigStore store(dir);
  auto base=store.initialize(firefly::config::ConfigLoader::fromFile(argc==3?argv[2]:"config/empty-home.json"));
  AdminTranscriber stt(std::make_unique<firefly::voice::SherpaRecognizer>("build/stt/model",firefly::voice::RecognitionProfile::AdminSetup));
  RuntimeModel model("/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm");
  RuntimeWorker worker(model);
  const auto call=[&](const Json&request){
   if(cancelled)throw std::runtime_error("acceptance_cancelled_before_call");
   std::cout<<"CALL_BEGIN "<<request.at("method")<<std::endl;
   const auto result=Json::parse(worker.handle(request.dump()));
   if(result.contains("trace")){report["test_model_responses"].push_back({{"method",request.at("method")},{"trace",result.at("trace")}});save();}
   std::cout<<"CALL_END "<<request.at("method")<<" ok="<<result.value("ok",false)<<std::endl;
   if(cancelled)throw std::runtime_error("acceptance_cancelled_after_call");
   return result;
  };
  auto initial=call({{"method","apply"},{"configuration",Json::parse(base->canonicalJson())}});
  if(!initial.value("ok",false))throw std::runtime_error(initial.dump());
  SetupSession setup(store,call);Json state=setup.handle("start",{});Json token=state.at("session_id");int seq=0;
  {
   std::ifstream recording("build/stt/vad-padding/300ms-06_kitchen_40.wav",std::ios::binary);
   if(!recording)throw std::runtime_error("DEV replay WAV missing");
   const std::string original((std::istreambuf_iterator<char>(recording)),{});
   const auto pcm=firefly::voice::decodeWavToSttPcm(original).samples;
   constexpr std::size_t samples=60*16000;std::string wav(44+2*samples,'\0');
   const auto put=[&](std::size_t offset,std::uint32_t value,int width){for(int i=0;i<width;++i)wav[offset+i]=static_cast<char>((value>>(8*i))&255);};
   wav.replace(0,4,"RIFF");put(4,wav.size()-8,4);wav.replace(8,8,"WAVEfmt ");put(16,16,4);put(20,1,2);put(22,1,2);put(24,16000,4);put(28,32000,4);put(32,2,2);put(34,16,2);wav.replace(36,4,"data");put(40,2*samples,4);
   for(std::size_t i=0;i<samples;++i)put(44+2*i,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(std::lround(pcm[i%pcm.size()]*32768),-32768L,32767L))),2);
   std::cout<<"ADMIN_STT_60S_BEGIN (repeated recorded DEV PCM, not live microphone)"<<std::endl;
   report["admin_stt_60s"]=Json::parse(stt.transcribe(wav,60));save();
   if(!report["admin_stt_60s"].value("ok",false))throw std::runtime_error("Real Admin 60-second STT failed");
   std::cout<<"ADMIN_STT_60S_END"<<std::endl;
  }
  const auto message=[&](const std::string& text){
   const auto started=std::chrono::steady_clock::now();
   state=setup.handle("message",{{"session_id",token},{"version",state.at("version")},{"request_id",std::to_string(++seq)},{"text",text}});
   std::ifstream progress(std::string(dir)+"/setup-draft.json");const auto storedProgress=Json::parse(progress);
   report["cases"].push_back({{"text",text},{"state",state},{"trace",storedProgress.at("trace")},{"steps",storedProgress.value("steps",Json::array())},{"wall_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()}});save();
   std::cout<<"CASE "<<seq<<" status="<<state.at("status")<<std::endl;
  };
  if(argc==2) {
  message("Создай детскую комнату. Там добавь телевизер где просто включаешь и отключаешь также добавь свет где только включаешь и отключаешь.");
  if(state.at("configuration").at("devices").size()!=2)throw std::runtime_error("initial graph not complete");
  for(const auto&d:state.at("configuration").at("devices"))if(d.at("action_ids").size()!=2||d.at("state").size()!=1)throw std::runtime_error("unrequested capabilities");
  message("Свет назови основным. Ещё добавь ночник только с включением и выключением.");
  if(state.at("configuration").at("devices").size()!=3)throw std::runtime_error("follow-up graph not complete");
  auto applied=setup.handle("apply",{{"session_id",token},{"version",state.at("version")}});report["first_apply"]=applied;save();
  if(applied.at("status")!="applied")throw std::runtime_error("Apply did not restore runtime");
  state=setup.handle("start",{});token=state.at("session_id");
  }
  message("Телевизор в детской назови экраном.");
  message("Переименуй его.");
  if(state.at("status")!="clarification_required")throw std::runtime_error("ambiguous pronoun was not rejected");
  message("Экран в детской назови телевизором.");
  if(state.at("status")!="editing")throw std::runtime_error("clarification did not resolve existing reference");
  message("Создай кабинет. Там добавь вентилятор только с включением и выключением. Создай кладовую. Там добавь лампу только с включением и выключением. Вентилятор в кабинете пока не добавляй.");
  if(state.at("status")!="editing")throw std::runtime_error("long request incomplete");
  const auto& final=state.at("configuration");
  if(final.at("rooms").size()!=3 || final.at("devices").size()!=4)throw std::runtime_error("long request final graph wrong");
  for(const auto& d:final.at("devices"))if(d.at("name").get<std::string>().find("ентилятор")!=std::string::npos)throw std::runtime_error("final exclusion lost");
  report["cancel"]=setup.handle("cancel",{{"session_id",token}});save();
  // Real compact inference after releasing admin. Uses the existing fixture's
  // schema, but never its HardwareExecutor or the user's configuration store.
  const auto fixture=firefly::config::ConfigLoader::fromFile("config/demo/v1-kitchen.json");
  firefly::runtime::DomainView domain(fixture);
  const std::string text="Поставь свет на кухне на сорок процентов";
  const auto selected=firefly::llm::selectAction(domain,text);
  const auto raw=model.inferCommand(text,firefly::llm::compactContract(domain,selected));
  report["runtime_compact"]={{"raw",raw},{"metrics",Json::parse(model.inferenceMetrics())}};save();
  std::ifstream wav("build/stt/vad-padding/300ms-06_kitchen_40.wav",std::ios::binary);
  if(wav){const std::string bytes((std::istreambuf_iterator<char>(wav)),{});report["stt_replay"]=Json::parse(stt.transcribe(bytes,60));save();}
  std::ifstream history(std::string(dir)+"/setup-draft.json");const auto stored=Json::parse(history);report["trace"]=stored.at("trace");
  if(raw!="[40]")throw std::runtime_error("runtime compact regression");
  if(!report.contains("stt_replay") || !report["stt_replay"].value("ok",false))throw std::runtime_error("real STT replay missing");
  report["complete"]=true;save();return 0;
 }catch(const std::exception&e){report["complete"]=false;report["error"]=e.what();save();std::cerr<<e.what()<<'\n';return 1;}
}
