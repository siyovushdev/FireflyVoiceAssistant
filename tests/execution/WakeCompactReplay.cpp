#include "firefly/admin/RuntimeWorker.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/voice/SherpaSpeech.h"
#include "RuntimeModel.h"
#include "c-api.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace firefly;
using namespace firefly::voice;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct Wake : WakeDetector {
    bool accept(const float* samples,int) override { return samples[0]==1; }
    void reset() override {}
};
struct Ack : WakeAcknowledgement {
    int polls=0;
    void start() override { polls=0; }
    AcknowledgementStatus poll() override { return ++polls<4 ? AcknowledgementStatus::Playing : AcknowledgementStatus::Complete; }
    void cancel() noexcept override {}
};
struct Source : AudioSource {
    std::function<void(float*,int)> next;
    int read(float* samples,int count) override { next(samples,count); return count; }
};
std::vector<float> recordedSpeech(SherpaVad& vad,const std::string& path) {
    std::unique_ptr<const SherpaOnnxWave,decltype(&SherpaOnnxFreeWave)> wave(SherpaOnnxReadWave(path.c_str()),SherpaOnnxFreeWave);
    require(wave && wave->sample_rate==SampleRate,"Invalid DEV recording");
    VoiceSegmenter segmenter(vad,{300,300,15000}); segmenter.startSession();
    std::vector<float> speech;
    for(int offset=0;offset<wave->num_samples;offset+=FrameSamples)
        for(const auto& u:segmenter.accept(wave->samples+offset,std::min(FrameSamples,wave->num_samples-offset))) {
            require(speech.empty() && u.error.empty(),"DEV recording did not contain exactly one bounded utterance"); speech=u.audio;
        }
    require(!speech.empty(),"No DEV speech"); return speech;
}
int main(int argc,char** argv) { try {
    if(argc!=7) throw std::runtime_error("Usage: wake_compact_replay STT VAD DEV_DIR RKLLM CONFIG REPORT");
    SherpaVad vad(argv[2]); SherpaRecognizer recognizer(argv[1]);
    const std::string dev=std::string(argv[3])+"/eval/";
    const auto kitchen=recordedSpeech(vad,dev+"06_kitchen_40.wav");
    const auto on=recordedSpeech(vad,dev+"04_light_on.wav");
    const auto off=recordedSpeech(vad,dev+"05_light_off.wav");
    const auto unknown=recordedSpeech(vad,dev+"02_open_door.wav");
    // DEV PCM composition only (no transcript repair): a recorded location prefix
    // plus recorded on/off. Original recordings remain untouched.
    std::vector<float> prefix;
    int prefixMs=0;
    for(int ms=400;ms<=1800;ms+=50) {
        if(ms*16>=static_cast<int>(kitchen.size())) break;
        std::vector<float> candidate(kitchen.begin(),kitchen.begin()+ms*16);
        if(recognizer.recognize(candidate).text=="на кухне") { prefix=std::move(candidate);prefixMs=ms; }
    }
    require(!prefix.empty(),"Cannot isolate recorded location prefix for DEV on/off fixture");
    const auto withLocation=[&](const std::vector<float>& audio) {
        auto value=prefix; value.insert(value.end(),1600,0);value.insert(value.end(),audio.begin(),audio.end());return value;
    };
    std::vector<std::vector<float>> clips{kitchen,withLocation(on),withLocation(off),unknown};
    std::cout << "Recorded location prefix=" << prefixMs << " ms; on=" << recognizer.recognize(clips[1]).text
        << "; off=" << recognizer.recognize(clips[2]).text << std::endl;
    const std::vector<int> expected{40,100,0,-1};
    admin::RuntimeModel model(argv[4]);admin::RuntimeWorker worker(model);
    const auto configuration=Json::parse(config::ConfigLoader::fromFile(argv[5])->canonicalJson());
    require(Json::parse(worker.handle(Json{{"method","apply"},{"configuration",configuration}}.dump())).at("ok"),"Runtime admission failed");
    Json report={{"recorded_prefix_ms",prefixMs},{"cases",Json::array()}};
    for(size_t index=0;index<clips.size();++index) {
        Wake wake;Ack ack;Source source;VoiceSegmenter segmenter(vad);WakeConfig config;
        Clock::time_point time{},armed{};std::string phase;int reads=0,waits=0,emitted=0;size_t offset=0;bool stop=false;
        Json events=Json::array(); VoiceResult result;
        source.next=[&](float* data,int count) {
            time+=std::chrono::milliseconds(32);std::fill(data,data+count,0);
            if(++reads>30000) throw std::runtime_error("Replay did not terminate");
            if(waits>=2 && reads>500) stop=true;
            if(reads==1) data[0]=1;
            else if(phase=="LISTENING_COMMAND" || phase=="CAPTURING_COMMAND") {
                if(time-armed>=std::chrono::milliseconds(4500)) {
                    const auto n=std::min(static_cast<size_t>(count),clips[index].size()-offset);
                    std::copy_n(clips[index].data()+offset,n,data);offset+=n;
                }
            } else if(phase=="PROCESSING") std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        runWakeVoiceLoop(source,wake,segmenter,config,
            [&](const Utterance& u){ return worker.processVoice(recognizer,u); },
            [&](const VoiceResult& value){result=value;++emitted;},
            [&](const std::string& state){phase=state;events.push_back({{"state",state},{"ms",std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count()}});
                if(state=="LISTENING_COMMAND")armed=time;if(state=="WAITING_WAKE")++waits;},
            [&]{return stop;},[&]{return time;},&ack);
        require(emitted==1 && waits==2,"Delayed speech was cut off or repeated without wake");
        const auto observed=Json::parse(worker.observe());
        std::cout << "Replay transcript " << index << ": " << result.recognition.text << " command=" << observed["last_command"].dump() << std::endl;
        if(expected[index]>=0) {
            const auto* action=std::get_if<actions::GenericAction>(&result.action);
            require(action && action->targetId()=="device_kitchen_light" && std::get<int64_t>(action->arguments().at("brightness").data())==expected[index],"Wrong replay action");
            require(observed["last_command"]["execution"]["success"],"HTTP mock execution failed");
            if(index==0) require(result.route=="LLM_FALLBACK","Brightness 40 did not exercise compact Qwen");
        } else require(std::holds_alternative<actions::Unsupported>(result.action) && observed["last_command"]["execution"].is_null(),"Unsupported speech executed");
        report["cases"].push_back({{"index",index},{"expected",expected[index]},{"transcript",result.recognition.text},{"route",result.route},
            {"processing_ms",result.processingMs},{"stt_ms",result.recognition.latencyMs},{"events",events},{"command",observed["last_command"]}});
        std::ofstream(argv[6])<<report.dump(2)<<'\n';
        std::cout<<"Replay "<<index<<" text="<<result.recognition.text<<" route="<<result.route<<" passed\n"<<std::flush;
    }
    worker.stop();
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
