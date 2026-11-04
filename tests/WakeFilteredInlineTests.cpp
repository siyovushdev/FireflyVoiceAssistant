// Production orchestration/VoiceProcessor/CommandEngine/SafetyGate; only audio,
// KWS, VAD and recognition are deterministic doubles (no actuator or model).
#include "TestSupport.h"
#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/llm/CompactActionDecoder.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
using namespace firefly;
using namespace firefly::voice;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Vad:SpeechDetector {
    int64_t received=0,start=-1,end=0;
    Detection accept(const float* p,int n)override {
        const auto previous=received;received+=n;
        const auto first=std::find_if(p,p+n,[](float v){return v>0;});
        if(first!=p+n){if(start<0)start=previous+(first-p);auto last=p+n;while(last>first&&last[-1]==0)--last;end=previous+(last-p);return {true,{}};}
        if(start>=0){auto b=start;start=-1;return {false,{{b,end}}};}return {};
    }
    void reset()override{received=end=0;start=-1;}
};
struct Source:AudioSource {
    std::function<void(float*,int)> next;
    int read(float* p,int n)override{next(p,n);std::this_thread::sleep_for(std::chrono::microseconds(100));return n;}
};
struct Wake:WakeDetector {
    int calls=0,trigger=40;bool second=false;
    bool accept(const float*,int)override{return ++calls==trigger || (second && calls==200);}
    void reset()override{}
};
struct Ack:WakeAcknowledgement {
    int starts=0;
    void start()override{++starts;}
    AcknowledgementStatus poll()override{return AcknowledgementStatus::Complete;}
    void cancel()noexcept override{}
};
struct Rec:SpeechRecognizer {
    std::string text;
    RecognitionResult recognize(const std::vector<float>&)override{return {true,text,{},0};}
};
struct Model:LlmEngine {std::string infer(std::string_view)override{return "[40]";}};
void scenario(const std::string& kind,const std::string& text="амина включи свет на кухне",bool allowed=true) {
    Source source;Wake wake;Ack ack;Vad vad;VoiceSegmenter segmenter(vad,{0,0,15000});
    Rec rec;rec.text=text;Model model;const auto& f=test::fixture();
    CommandEngine engine(model,f.parser,f.domain,[&](std::string_view raw){return llm::CompactActionDecoder(f.domain,{"device_kitchen_light","action_set_brightness"}).decode(raw);});
    VoiceProcessor processor(rec,engine);
    std::atomic<int> reads{0};std::atomic<bool> entered{false},finished{false};
    std::atomic<bool> stop{false};int processed=0,emitted=0,waits=0,rejected=0;std::string phase;int afterBeepFrames=0;
    auto time=std::chrono::steady_clock::time_point{};
    WakeInputPolicy policy{false,7};
    wake.second=kind=="switch";const bool wrapped=kind=="wrapped";const int shift=wrapped?100:0;wake.trigger+=shift;
    const int commandStart=(kind=="late"?53:42)+shift,commandEnd=60+shift;
    source.next=[&](float* p,int n){
        const int frame=++reads;check(frame<1500,"Filtered inline stalled");time+=std::chrono::milliseconds(32);
        if(kind=="switch" && entered && frame>=50)policy={true,8};
        float value=0;
        if(frame>=30+shift && frame<=40+shift)value=.3f;
        if(frame>=commandStart && frame<=commandEnd)value=.6f;
        if(kind=="switch" && (phase=="LISTENING_COMMAND" || phase=="CAPTURING_COMMAND"))value=++afterBeepFrames<=10?.6f:0;
        std::fill(p,p+n,value);
        if((kind=="reject" || kind=="error" || kind=="timeout") && rejected)stop=true;
        
        if(kind=="stop" && entered)stop=true;
        if(kind=="name" && phase=="LISTENING_COMMAND")stop=true;
    };
    runWakeVoiceLoop(source,wake,segmenter,{},[&](const Utterance& u){
        ++processed;check(finished,"Command ran concurrently with wake STT");
        check(u.wakeAdmitted,"Lost admission");
        if(kind=="switch"){check(!u.inlineWake && u.inputGeneration==8 && ack.starts==1 && u.audio==std::vector<float>(10*FrameSamples,.6f),"New mode did not use fresh post-beep PCM");rec.text="включи свет на кухне";}
        else check(u.inlineWake && u.inputGeneration==7,"Lost inline generation");
        if(kind!="name" && kind!="switch"){
            std::vector<float> expected(11*FrameSamples,.3f);
            expected.insert(expected.end(),(commandStart-(41+shift))*FrameSamples,0);
            expected.insert(expected.end(),(commandEnd-commandStart+1)*FrameSamples,.6f);
            if(u.audio!=expected)throw std::runtime_error(kind+": lost/duplicated PCM; actual="+std::to_string(u.audio.size())+" expected="+std::to_string(expected.size()));
        }
        return processor.process(u);
    },[&](const VoiceResult& r){
        ++emitted;
        if(kind=="name"){check(r.wakeOnly,"Wake alone did not re-beep");return;}
        check(allowed ? std::holds_alternative<actions::GenericAction>(r.action) : std::holds_alternative<actions::Unsupported>(r.action),"Safety result wrong for original transcript");
        stop=true;
    },[&](const std::string& p){phase=p;if(p=="WAITING_WAKE")++waits;if(p=="WAKE_REJECTED")++rejected;},[&]{return stop.load();},[&]{return time;},&ack,[&](const std::vector<float>& pcm){
        entered=true;check(pcm.size()>=8*FrameSamples,"Missing wake tail");
        while(reads<67+shift && !stop)std::this_thread::yield();
        if(kind=="timeout")while(reads<110+shift)std::this_thread::yield();
        finished=true;if(kind=="error")throw std::runtime_error("STT error");return kind!="reject";
    },[&]{return policy;});
    check(finished,"Verifier not joined");
    if(kind=="stop" || kind=="reject" || kind=="error" || kind=="timeout")
        check(processed==0 && emitted==0 && ack.starts==0,"Unadmitted/cancelled audio executed or beeped");
    else check(processed==1 && emitted==1 && ack.starts==((kind=="name" || kind=="switch")?1:0),"Inline not exactly once or wrong beep");
}
void segmentHistory(){
    Vad vad;VoiceSegmenter segmenter(vad,{0,0,1000});
    Utterance bad{{10,100},std::vector<float>(90),{}};
    check(!segmenter.includeEarlierSpeech(bad,0).error.empty(),"Unavailable history accepted");
}
}
int main(){try{
    segmentHistory();for(const auto* kind:{"immediate","late","wrapped","switch","stop","reject","error","timeout"})scenario(kind);
    scenario("name","амина");
    for(const auto* text:{"амина не включай свет на кухне","амина да я включил свет на кухне","амина да горит свет на кухне","включи свет на кухне"})scenario("late",text,false);
    scenario("late","афина выключи свет на кухне");
    std::cout<<"Filtered inline: exact buffered PCM/gap, delayed STT, ring wrap, cancellation, deadlines, wake-only beep, malicious-model safety and positive controls passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
