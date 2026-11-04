// Explicit saved-PCM integration tool. No microphone, model inference for
// commands, RuntimeWorker or device executors. KWS admission uses recorded
// production candidate coordinates; actual STT/VAD and the loop are unchanged.
#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/voice/WakeConfirmation.h"
#include "firefly/voice/SherpaSpeech.h"
#include "c-api.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
using namespace firefly::voice;
using Json=nlohmann::json;
struct Input:AudioSource {
    std::vector<float> pcm;std::size_t offset=0;bool done=false;
    int read(float* out,int n)override{
        std::fill(out,out+n,0);const auto count=std::min<std::size_t>(n,offset<pcm.size()?pcm.size()-offset:0);
        if(count)std::copy_n(pcm.data()+offset,count,out);offset+=n;
        std::this_thread::sleep_for(std::chrono::milliseconds(32));
        if(offset>pcm.size()+2*SampleRate)done=true;return n;
    }
};
struct Candidate:WakeDetector {int calls=0,block=0;bool accept(const float*,int)override{return ++calls==block;}void reset()override{}};
struct Ack:WakeAcknowledgement{int count=0;void start()override{++count;}AcknowledgementStatus poll()override{return AcknowledgementStatus::Complete;}void cancel()noexcept override{}};
int main(int argc,char** argv){try{
    if(argc!=5)throw std::runtime_error("Usage: wake_filtered_replay STT_DIR VAD_MODEL INPUT_JSON REPORT_JSON");
    SherpaRecognizer rec(argv[1]);SherpaVad vad(argv[2]);SttWakeConfirmation verifier(rec);
    Json inputs;std::ifstream(argv[3])>>inputs;Json rows=Json::array();
    for(auto row:inputs){
        Input source;const auto* wave=SherpaOnnxReadWave(row.at("path").get<std::string>().c_str());
        if(!wave || wave->sample_rate!=SampleRate)throw std::runtime_error("Invalid wave");
        source.pcm.assign(wave->samples,wave->samples+wave->num_samples);SherpaOnnxFreeWave(wave);
        Candidate wake;wake.block=row.at("trigger_block");Ack ack;VoiceSegmenter segmenter(vad);
        bool stop=false;int processed=0;std::string text,command;Json states=Json::array(),confirmation;
        VoiceProcessor processor(rec,[&](std::string_view value){command=std::string(value);return firefly::CommandResult{};});
        const auto started=std::chrono::steady_clock::now();
        runWakeVoiceLoop(source,wake,segmenter,{},[&](const Utterance& u){++processed;return processor.process(u);},[&](const VoiceResult& r){text=r.recognition.text;if(!r.wakeOnly)stop=true;},[&](const std::string& value){states.push_back(value);if(value=="LISTENING_COMMAND" || value=="WAKE_REJECTED")stop=true;},[&]{return stop||source.done;},std::chrono::steady_clock::now,&ack,[&](const std::vector<float>& pcm){auto r=verifier.confirm(pcm);confirmation={{"accepted",r.accepted},{"reason",r.reason},{"processing_ms",r.processingMs}};return r.accepted;});
        row["confirmation"]=confirmation;row["text"]=text;row["command"]=command;row["processed"]=processed;row["beeps"]=ack.count;row["states"]=states;row["wall_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();rows.push_back(row);
        std::cout<<row.dump()<<'\n'<<std::flush;
    }
    std::ofstream(argv[4])<<Json({{"scope","real saved audio/STT/VAD, recorded KWS admission; no commands executed"},{"rows",rows}}).dump(2)<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
