#include "firefly/voice/WakeConfirmation.h"
#include "firefly/voice/WakeVoiceLoop.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace firefly::voice;
namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct Recognizer : SpeechRecognizer {
    std::vector<RecognitionResult> replies;
    std::vector<std::vector<float>> inputs;
    RecognitionResult recognize(const std::vector<float>& pcm) override {
        inputs.push_back(pcm);
        return replies.at(inputs.size()-1);
    }
};
RecognitionResult text(const char* value) { RecognitionResult r; r.success=true; r.text=value; return r; }
void recognition() {
    for (const auto* value : {"амина", "Афина!", "а мина", "АМИНА, включи свет", "АфИна"})
        check(SttWakeConfirmation::matches(value), "Documented name/alias rejected");
    for (const auto* value : {"", "шум", "салют у нас", "в", "програл снеси меня", "витамина", "аминов", "аминар", "аминок", "мира", "Амила", "алина"})
        check(!SttWakeConfirmation::matches(value), "Unrelated text silently repaired into name");
    Recognizer recognizer; SttWakeConfirmation verifier(recognizer);
    std::vector<float> pcm(3*SampleRate); for (std::size_t i=0;i<pcm.size();++i) pcm[i]=float(i)/pcm.size();
    recognizer.replies={text("выходить на сами на"),text("амина")};
    auto r=verifier.confirm(pcm);
    check(r.accepted && r.attempts.size()==2 && r.attempts.front().text=="выходить на сами на", "Tail did not recover name without altering transcript");
    check(recognizer.inputs[0]==pcm && recognizer.inputs[1]==std::vector<float>(pcm.end()-25600,pcm.end()), "Wake PCM altered or tail wrong");
    recognizer.inputs.clear();recognizer.replies={text("афина включи свет")};r=verifier.confirm(pcm);
    check(r.accepted && recognizer.inputs.size()==1, "Confirmed name unnecessarily decoded twice");
    recognizer.inputs.clear();recognizer.replies={text("салют у нас"),text("смотри")};r=verifier.confirm(pcm);
    check(!r.accepted && r.reason=="name_not_found", "Noise reached admission");
    recognizer.inputs.clear();recognizer.replies={RecognitionResult{}};r=verifier.confirm(pcm);
    check(!r.accepted && r.reason=="stt_unavailable" && recognizer.inputs.size()==1, "STT error reused stale name");
    recognizer.inputs.clear();recognizer.replies={text("шум"),RecognitionResult{}};r=verifier.confirm(pcm);
    check(!r.accepted && r.reason=="stt_unavailable", "Second STT error admitted wake");
    const auto count=recognizer.inputs.size();
    for (auto bad : {std::vector<float>{},std::vector<float>(3*SampleRate+8*FrameSamples+1),std::vector<float>{1.1f},std::vector<float>{std::numeric_limits<float>::quiet_NaN()}})
        check(!verifier.confirm(bad).accepted,"Invalid PCM accepted");
    check(count==recognizer.inputs.size(), "Invalid PCM reached recognizer");
}
struct Source : AudioSource {
    std::function<void(float*,int)> next;
    int read(float* p,int n) override {next(p,n);std::this_thread::sleep_for(std::chrono::microseconds(50));return n;}
};
struct Wake : WakeDetector {
    int calls=0,resets=0;
    bool accept(const float*,int) override {++calls;return calls==140 || calls==200;}
    void reset() override {++resets;}
};
struct Vad : SpeechDetector {
    std::string& phase; int64_t received=0,start=-1,end=0;
    explicit Vad(std::string& p):phase(p){}
    Detection accept(const float* p,int n) override {
        check(phase=="LISTENING_COMMAND" || phase=="CAPTURING_COMMAND","Verification PCM entered command VAD");
        check(p[0]==.55f || p[0]==0,"Room/beep/busy PCM entered command VAD");
        const auto before=received;received+=n;
        if(p[0]!=0){if(start<0)start=before;end=received;return {true,{}};}
        if(start>=0){auto s=start;start=-1;return {false,{{s,end}}};}return {};
    }
    void reset() override {received=end=0;start=-1;}
};
struct Ack : WakeAcknowledgement {
    int starts=0;
    void start() override {++starts;}
    AcknowledgementStatus poll() override {return AcknowledgementStatus::Complete;}
    void cancel() noexcept override {}
};
void loop(const std::string& kind) {
    Source source;Wake wake;Ack ack;std::string phase;Vad vad(phase);VoiceSegmenter segmenter(vad,{0,0,1000});
    WakeConfig config;config.afterBeepOnly=true;
    std::atomic<int> jobs{0},running{0};std::atomic<bool> release{false};
    auto now=std::chrono::steady_clock::time_point{};
    bool stop=false;int reads=0,waits=0,confirmed=0,rejected=0,timeouts=0,processed=0,emitted=0,speechFrames=0,busyFrames=0;
    source.next=[&](float* data,int n){
        check(++reads<10000,"Confirmation replay stalled");now+=std::chrono::milliseconds(32);
        float value=.25f;
        if(phase=="VERIFYING_WAKE" || phase=="WAKE_CONFIRMATION_TIMEOUT") {
            value=.33f;
            if(running.load()) {
                ++busyFrames;
                check(wake.calls==(jobs==1?140:200),"New KWS candidate queued while verifier busy");
                if(kind=="stop"){stop=true;release=true;}
                if(busyFrames>=75)release=true;
            }
        }
        if(phase=="LISTENING_COMMAND" || phase=="CAPTURING_COMMAND")value=++speechFrames<=4?.55f:0;
        std::fill(data,data+n,value);
    };
    const auto verifier=[&](const std::vector<float>& pcm) {
        const auto job=++jobs;++running;
        struct Exit {std::atomic<int>& n;~Exit(){--n;}} exit{running};
        check(pcm.size()<=3*SampleRate+4096 && pcm.size()>=4096,"Unbounded verification audio");
        check(std::all_of(pcm.end()-4096,pcm.end(),[](float v){return v==.33f;}),"Real 256ms tail not preserved");
        if(job==1) {
            if(kind=="timeout" || kind=="stop")while(!release.load())std::this_thread::yield();
            if(kind=="exception")throw std::runtime_error("STT failed");
            return kind=="timeout" || kind=="stop";
        }
        return true;
    };
    runWakeVoiceLoop(source,wake,segmenter,config,[&](const Utterance& u){
        ++processed;check(confirmed==1 && ack.starts==1,"Command admitted before confirmed beep");
        check(!u.inlineWake && !u.audio.empty() && u.audio.front()==.55f,"Wake/noise PCM became a command");return VoiceResult{};
    },[&](const VoiceResult&){++emitted;},[&](const std::string& p){
        phase=p;if(p=="WAITING_WAKE" && ++waits==3)stop=true;
        if(p=="WAKE_CONFIRMED")++confirmed;
        if(p=="WAKE_REJECTED")++rejected;
        if(p=="WAKE_CONFIRMATION_TIMEOUT")++timeouts;
    },[&]{return stop;},[&]{return now;},&ack,verifier);
    check(running==0,"Verifier escaped loop teardown");
    if(kind=="stop")check(ack.starts==0 && processed==0 && emitted==0,"Stop beeped or executed");
    else {
        check(jobs==2 && rejected==1 && confirmed==1 && ack.starts==1 && processed==1 && emitted==1,"Rejected wake leaked or next legitimate wake failed");
        check(timeouts==(kind=="timeout"?1:0),"Late verification not rejected once");
    }
}
}
int main(){try{
    recognition();for(const auto* kind:{"reject","exception","timeout","stop"})loop(kind);
    std::cout<<"Wake confirmation: names, noise, exact PCM/tail, errors, deadline, single slot, cancellation and next-command isolation passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
