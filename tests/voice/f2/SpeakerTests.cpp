// Real Speaker concurrency/state machine; synthetic TTS/ALSA dependencies.
#include "F2Worker.h"
using namespace firefly::voice::f2;
#include <cassert>
#include <iostream>
#include <limits>
#include <unistd.h>
struct SherpaOnnxOfflineTts {};
static std::atomic<int> loads{0},models{0},audios{0},requests{0},writes{0};
static std::mutex traceMutex;
static std::vector<std::string> requestsText;
extern "C" {
const SherpaOnnxOfflineTts* SherpaOnnxCreateOfflineTts(const SherpaOnnxOfflineTtsConfig* c){assert(c->model.num_threads==4);++loads;++models;return new SherpaOnnxOfflineTts;}
void SherpaOnnxDestroyOfflineTts(const SherpaOnnxOfflineTts* p){delete p;--models;}
const SherpaOnnxGeneratedAudio* SherpaOnnxOfflineTtsGenerateWithConfig(const SherpaOnnxOfflineTts*,const char* text,const SherpaOnnxGenerationConfig* g,SherpaOnnxGeneratedAudioProgressCallbackWithArg callback,void* arg) {
    assert(g->sid==1&&g->num_steps==5&&g->speed==1);++requests;
    {std::lock_guard<std::mutex> l(traceMutex);requestsText.emplace_back(text);}
    for(int i=0;i<(std::string(text)=="SLOW."?100:5);++i){std::this_thread::sleep_for(std::chrono::milliseconds(1));if(!callback(nullptr,0,0,arg))return nullptr;}
    if(std::string(text)=="FAILTTS.")return nullptr;
    auto samples=new float[128];std::fill(samples,samples+128,.2f);
    if(std::string(text)=="LOUD.")samples[0]=1.5f;
    if(std::string(text)=="NAN.")samples[0]=std::numeric_limits<float>::quiet_NaN();
    ++audios;return new SherpaOnnxGeneratedAudio{samples,128,44100};
}
void SherpaOnnxDestroyOfflineTtsGeneratedAudio(const SherpaOnnxGeneratedAudio* p){delete[] p->samples;delete p;--audios;}
int32_t SherpaOnnxWriteWave(const float* data,int32_t n,int32_t,const char* path) {
    for(int i=0;i<n;++i)assert(std::isfinite(data[i])&&std::abs(data[i])<=.96001f);
    ++writes;std::ofstream(path)<<"synthetic dependency output";return 1;
}
}
int main() {
    namespace fs=std::filesystem;
    fs::path root=fs::temp_directory_path()/("f2-speaker-"+std::to_string(getpid()));fs::create_directory(root);
    int checks=0;
    {
        Events events([](const std::string&) {});
        Speaker speaker(events,"CPU_DOUBLE",(root/"speech").string(),"CPU_DOUBLE",true);
        auto turn=[&](int id,std::string text,bool expected){speaker.begin(id);speaker.push(text);bool ok=speaker.finish(true);if(ok!=expected)std::cerr<<"Unexpected result for turn "<<id<<'\n';assert(ok==expected);++checks;};
        turn(1,"Первая. Вторая.",true);
        assert(loads==1&&writes==2);++checks;
        turn(2,"<think>SECRET.</think>Ответ. ```cpp\nCODE.\n```",true);
        {std::lock_guard<std::mutex> l(traceMutex);for(auto& x:requestsText)assert(x.find("SECRET")==std::string::npos&&x.find("CODE")==std::string::npos);}++checks;
        turn(3,"FAILTTS.",false);turn(4,"Снова работает.",true);
        turn(5,"NAN.",false);turn(6,"Снова работает.",true);
        turn(7,"LOUD.",true);
        std::string many;for(int i=0;i<100;++i)many+="Х. ";
        turn(8,many,false);turn(9,"После переполнения.",true);
        speaker.begin(10);speaker.push("SLOW.");std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!speaker.finish(false));++checks;
        turn(11,"После отмены.",true);
        assert(loads==1);++checks;
    }
    assert(models==0&&audios==0&&fake_alsa_handles==0);++checks;
    {
        Events events([](const std::string&) {});
        Speaker speaker(events,"CPU_DOUBLE",(root/"play-error").string(),"FAIL_PLAY",false);
        speaker.begin(1);speaker.push("Ошибка устройства.");assert(!speaker.finish(true));++checks;
    }
    {
        Events events([](const std::string&) {});auto start=Clock::now();
        auto s=std::make_unique<Speaker>(events,"CPU_DOUBLE",(root/"stop").string(),"CPU_DOUBLE",false);
        s->begin(1);s->push("SLOW.");std::this_thread::sleep_for(std::chrono::milliseconds(10));s.reset();
        assert(Clock::now()-start<std::chrono::seconds(1));++checks;
    }
    {
        Events events([](const std::string&) {});
        Speaker speaker(events,"CPU_DOUBLE",(root/"drain-error").string(),"FAIL_DRAIN",false);
        speaker.begin(1);speaker.push("Проверка ошибки завершения.");assert(!speaker.finish(true));++checks;
    }
    {
        Events events([](const std::string&) { throw std::runtime_error("log sink failed"); });
        Speaker speaker(events,"CPU_DOUBLE",(root/"cancel-api").string(),"CPU_DOUBLE",false);
        auto start=Clock::now();speaker.begin(1);speaker.push("SLOW.");
        assert(!speaker.finish(true,[&]{return Clock::now()-start>std::chrono::milliseconds(10);}));++checks;
        assert(speaker.cancelled()&&Clock::now()-start<std::chrono::seconds(1));++checks;
        speaker.begin(2);speaker.push("После отмены.");assert(speaker.finish(true));++checks;
    }
    assert(models==0&&audios==0&&fake_alsa_handles==0);++checks;
    fs::remove_all(root);
    std::cout<<"PASS "<<checks<<" speaker state/concurrency checks; TTS+ALSA doubles; no real playback\n";
}
