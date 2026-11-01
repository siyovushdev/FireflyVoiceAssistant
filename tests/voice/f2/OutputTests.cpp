// Actual public F2 output + embedded PCM, real SHA/path validation; only SDK/ALSA are doubles.
#include "firefly/voice/F2SpeechOutput.h"
#include "alsa/asoundlib.h"
#include "c-api.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace firefly::voice;
static std::atomic<int> sdkCalls{0};
extern "C" {
const SherpaOnnxOfflineTts* SherpaOnnxCreateOfflineTts(const SherpaOnnxOfflineTtsConfig*){++sdkCalls;return nullptr;}
void SherpaOnnxDestroyOfflineTts(const SherpaOnnxOfflineTts*){}
const SherpaOnnxGeneratedAudio* SherpaOnnxOfflineTtsGenerateWithConfig(const SherpaOnnxOfflineTts*,const char*,const SherpaOnnxGenerationConfig*,SherpaOnnxGeneratedAudioProgressCallbackWithArg,void*){++sdkCalls;return nullptr;}
void SherpaOnnxDestroyOfflineTtsGeneratedAudio(const SherpaOnnxGeneratedAudio*){}
int32_t SherpaOnnxWriteWave(const float*,int32_t,int32_t,const char*){throw std::runtime_error("unexpected recording");}
}
int main(){
 int guardCalls=0,events=0;
 {
  F2SpeechOutput output("/missing-f2-model","CPU_DOUBLE",[&](const std::string&){++events;},"",
      [&](const std::function<void()>& load){++guardCalls;load();});
  for(int i=0;i<10;++i)assert(output.speak("Команда принята исполнителем. Это не подтверждает состояние устройства.",{}).success);
  assert(guardCalls==0&&sdkCalls==0&&fake_alsa_writes>0&&events>0);
  const auto writes=fake_alsa_writes.load();
  assert(output.speak("Команда принята исполнителем.",[]{return true;}).cancelled);
  assert(fake_alsa_writes==writes);
  auto dynamic=output.speak("Новое неизвестное предложение.",{});
  assert(!dynamic.success&&!dynamic.error.empty()&&guardCalls==1&&sdkCalls==0);
  assert(output.speak("Команда не отправлена.",{}).success);
 }
 assert(fake_alsa_handles==0);
 {
  F2SpeechOutput output("/missing-f2-model","FAIL_PLAY");
  const auto r=output.speak("Команда не отправлена.",{});
  assert(!r.success&&!r.error.empty()&&sdkCalls==0);
 }
 std::cout<<"PASS public output: cold stock without model/guard/SDK, 10 repeats, pending stop, dynamic asset failure, next stock, playback failure; fake ALSA only\n";
}
