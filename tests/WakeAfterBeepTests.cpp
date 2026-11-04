#include "firefly/voice/WakeVoiceLoop.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace firefly;
using namespace firefly::voice;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
struct Capture : AudioSource {
    std::function<float()> next;
    int read(float* data,int count) override { std::fill(data,data+count,next());std::this_thread::yield();return count; }
};
struct Wake : WakeDetector {
    int calls=0;
    bool accept(const float*,int) override { return ++calls==12; }
    void reset() override {}
};
struct Vad : SpeechDetector {
    std::string& phase;int64_t received=0,start=-1,end=0;
    explicit Vad(std::string& phase):phase(phase){}
    Detection accept(const float* data,int count) override {
        check(phase=="LISTENING_COMMAND"||phase=="CAPTURING_COMMAND","Room/beep entered continuation VAD");
        check(data[0]!=.9f,"Beep entered VAD");
        auto previous=received;received+=count;
        if(data[0]!=0) {if(start<0)start=previous;end=received;return {true,{}};}
        if(start>=0) {auto begin=start;start=-1;return {false,{{begin,end}}};}
        return {};
    }
    void reset() override {received=end=0;start=-1;}
};
struct Ack : WakeAcknowledgement {
    int starts=0,polls=0,cancels=0;bool fail=false;
    void start() override {++starts;polls=0;}
    AcknowledgementStatus poll() override {return fail?AcknowledgementStatus::Failed:++polls>=3?AcknowledgementStatus::Complete:AcknowledgementStatus::Playing;}
    void cancel() noexcept override {++cancels;}
};
struct Recognizer : SpeechRecognizer {
    std::string text;
    RecognitionResult recognize(const std::vector<float>&) override {return {true,text,{},0};}
};
void scenario(const std::string& name) {
    Capture capture;Wake wake;std::string phase;Vad vad(phase);Ack ack;
    ack.fail=name=="failed-beep";
    VoiceSegmenter segmenter(vad,{0,0,1000});WakeConfig config;config.afterBeepOnly=true;
    config.commandStartTimeout=std::chrono::milliseconds(1000);
    auto now=std::chrono::steady_clock::time_point{};
    int reads=0,waits=0,listens=0,speechFrames=0,emitted=0,handlers=0;
    bool stopped=false;std::atomic<int> processed{0};
    Recognizer recognizer;
    VoiceProcessor processor(recognizer,[&](std::string_view text) {
        ++handlers;check(text=="включи свет","Wake-only text reached command path");return CommandResult{};
    });
    capture.next=[&] {
        now+=std::chrono::milliseconds(32);check(++reads<100000,"Unbounded session");
        if(waits>=2)stopped=true;
        if(name=="stop-beep"&&phase=="BEEP_START")stopped=true;
        if(phase=="BEEP_START"||phase=="BEEP_END")return .9f;
        if(phase=="WAITING_WAKE")return .7f; // Continuous room/inline speech before wake decision.
        if(phase=="LISTENING_COMMAND"||phase=="CAPTURING_COMMAND") {
            if(name=="inline-only"||name=="no-command")return 0.f;
            if(name=="long-room-speech")return .7f;
            return ++speechFrames<=8?.3f:0.f;
        }
        return 0.f;
    };
    runWakeVoiceLoop(capture,wake,segmenter,config,[&](const Utterance& u) {
        const auto count=++processed;
        check(!u.inlineWake&&u.wakeAdmitted,"After-beep admission flags wrong");
        check(std::all_of(u.audio.begin(),u.audio.end(),[](float x){return x==.3f;}),"Pre-beep room/inline/echo PCM reached STT");
        recognizer.text=name=="repeat"&&count==1?"Амина, Амина!":"включи свет";
        return processor.process(u);
    },[&](const VoiceResult&){++emitted;},[&](const std::string& state) {
        phase=state;
        check(state!="CHECKING_CONTINUATION"&&state!="CONTINUATION_INLINE","After-beep silently chose inline path");
        if(state=="WAITING_WAKE")++waits;
        if(state=="LISTENING_COMMAND") {++listens;speechFrames=0;}
    },[&]{return stopped;},[&]{return now;},&ack);
    check(ack.cancels>0,"Beep output not cancelled at shutdown");
    if(name=="failed-beep"||name=="stop-beep"||name=="inline-only"||name=="no-command"||name=="long-room-speech") {
        check(processed==0&&handlers==0,"Unacknowledged/pre-beep/oversized speech reached command processing");
        if(name=="failed-beep")check(listens==0,"Failed beep silently opened command capture");
    } else if(name=="repeat")check(processed==2&&handlers==1&&ack.starts==2&&emitted==2,"Wake repetition did not re-beep without execution");
    else check(processed==1&&handlers==1&&ack.starts==1&&emitted==1,"Exactly one acknowledged command required");
}
void wakeOnly() {
    Recognizer r;int calls=0;VoiceProcessor p(r,[&](std::string_view){++calls;return CommandResult{};});
    Utterance u{{0,512},std::vector<float>(512),{},false,true};
    for(const auto* text:{"амина","Амина!","амина амина","Амина, АМИНА?","Афина!"}) {
        r.text=text;auto out=p.process(u);check(out.wakeOnly&&out.error.empty()&&out.route=="WAKE_ONLY"&&calls==0,"Only wake executed or produced speech error");
    }
    for(const auto* text:{"Аминат","не амина","свет амина","амина включи свет","включи свет"}) {
        r.text=text;const auto before=calls;auto out=p.process(u);check(!out.wakeOnly&&calls==before+1,"Command text incorrectly rewritten as wake-only");
    }
    u.wakeAdmitted=false;r.text="амина";const auto before=calls;
    check(!p.process(u).wakeOnly&&calls==before+1,"Non-KWS text received wake control authority");
}
}
int main() {try {
    for(const auto* name:{"command","repeat","inline-only","no-command","long-room-speech","failed-beep","stop-beep"})scenario(name);
    wakeOnly();std::cout<<"PASS 7 after-beep scenarios + exact wake-only admission checks\n";
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
