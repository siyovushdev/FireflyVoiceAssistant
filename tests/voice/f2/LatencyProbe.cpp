// Explicitly invoked native playback benchmark. Never registered in CTest or installed.
// Synthetic spoken fixtures only. No microphone, RKLLM, transport or ConfigStore.
#include "firefly/voice/F2SpeechOutput.h"
#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
using firefly::voice::F2SpeechOutput;
using Clock=std::chrono::steady_clock;
using Json=nlohmann::json;
int main(int argc,char** argv) {
 try {
  if(argc!=3)throw std::runtime_error("usage: f2_latency_probe PINNED_MODEL_DIRECTORY EXPLICIT_ALSA_DEVICE");
  int id=0;std::atomic<double> first{0};Clock::time_point start;
  auto sink=[&](const std::string& row){auto j=Json::parse(row);j["probe_id"]=id;std::cout<<j.dump()<<std::endl;
   if(j["event"]=="first_playback")first=std::chrono::duration<double,std::milli>(Clock::now()-start).count();};
  auto speak=[&](F2SpeechOutput& output,const std::string& phase,const std::string& text,bool cancel=false,bool fast=false,Clock::time_point ready={}){
   ++id;first=0;start=ready==Clock::time_point{}?Clock::now():ready;
   auto r=output.speak(text,[&]{return cancel&&first>0&&Clock::now()-start>std::chrono::milliseconds(static_cast<int>(first.load())+250);});
   if(cancel?(!r.cancelled||r.success):(!r.success||first==0))throw std::runtime_error("speech failed "+phase+": "+r.error);
   if(fast&&first>=1000)throw std::runtime_error("first PCM exceeded 1s target");
   std::cout<<Json{{"event","probe_result"},{"id",id},{"phase",phase},{"text",text},{"first_pcm_ms",first.load()},
    {"total_ms",std::chrono::duration<double,std::milli>(Clock::now()-start).count()},{"cancelled",r.cancelled}}.dump()<<std::endl;
  };
  const std::string stock="Команду отправила, ответа пока нет.";
  for(int i=0;i<10;++i){auto ready=Clock::now();F2SpeechOutput cold("/intentionally/missing-model",argv[2],sink);speak(cold,"cold_stock",stock,false,true,ready);}
  F2SpeechOutput output(argv[1],argv[2],sink);
  for(int i=0;i<10;++i)speak(output,"warm_stock",stock,false,true);
  for(int i=0;i<10;++i)speak(output,"new_short","«Лампа номер "+std::to_string(i)+"»: состояние неизвестно.");
  for(int i=0;i<10;++i)speak(output,"cached_short","«Лампа номер "+std::to_string(i)+"»: состояние неизвестно.",false,true);
  for(int i=0;i<10;++i)speak(output,"new_long","«Лампа номер "+std::to_string(i)+"»: питание: по подтверждённому наблюдению «включено». Доступность канала неизвестна.");
  speak(output,"cancel_stock",stock,true);
  speak(output,"after_cancel",stock,false,true);
  auto failure=output.speak("Новое предложение.",[]{return true;});if(!failure.cancelled)throw std::runtime_error("pending stop failed");
  std::cout<<"NATIVE_LATENCY_PROBE_PASS synthetic fixture text; first successful ALSA write, not acoustic microphone timing\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
