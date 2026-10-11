#include "TestSupport.h"
#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/voice/WakePcmRing.h"
#include "firefly/llm/CompactActionDecoder.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
using namespace firefly;
using namespace firefly::voice;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Vad : SpeechDetector {
    int64_t received = 0, start = -1, end = 0;
    Detection accept(const float* pcm, int n) override {
        const auto previous = received; received += n;
        require(pcm[0] != .9f, "Beep entered VAD");
        const auto first = std::find_if(pcm,pcm+n,[](float value) { return value > 0; });
        if (first != pcm+n) {
            if (start < 0) start = previous + (first-pcm);
            auto last = pcm+n;
            while (last > first && last[-1] == 0) --last;
            end = previous + (last-pcm);
            return {end - start >= 4096, {}};
        }
        if (start >= 0) { auto begin = start; start = -1; return {false, {{begin, end}}}; }
        return {};
    }
    void reset() override { received = end = 0; start = -1; }
};
struct Wake : WakeDetector {
    int calls = 0, trigger = 0;
    bool accept(const float*, int) override { return ++calls == trigger; }
    void reset() override {}
};
struct Ack : WakeAcknowledgement {
    int starts = 0, polls = 0, cancels = 0;
    void start() override { ++starts; }
    AcknowledgementStatus poll() override { return ++polls == 4 ? AcknowledgementStatus::Complete : AcknowledgementStatus::Playing; }
    void cancel() noexcept override { ++cancels; }
};
struct Source : AudioSource {
    std::function<float()> next;
    int read(float* pcm, int n) override { std::fill(pcm, pcm + n, next()); std::this_thread::yield(); return n; }
};
struct Recognizer : SpeechRecognizer {
    std::string text;
    RecognitionResult recognize(const std::vector<float>&) override { return {true,text,{},0}; }
};
struct Model : LlmEngine {
    int calls = 0;
    std::string infer(std::string_view) override { ++calls; return "[40]"; }
};
void prefixTests() {
    Recognizer recognizer;
    int calls = 0; std::string received;
    VoiceProcessor processor(recognizer, [&](std::string_view text) { ++calls; received = text; return CommandResult{}; });
    Utterance u{{0,512},std::vector<float>(512),{},true};
    for (const auto* prefix : {"амина ","Амина, ","АМИНА: ","афина ","Афина! "}) {
        recognizer.text = std::string(prefix) + "включи свет на кухне";
        auto result = processor.process(u);
        require(result.wakePrefixRemoved && received == "включи свет на кухне" && result.recognition.text == recognizer.text,
                "Exact prefix not removed or raw STT changed");
    }
    for (const auto* text : {"", "амина", "афина!", "аминат включи свет", "амин включи свет", "свет амина на кухне", "hello", "аминавключи", "\xff"}) {
        recognizer.text = text; const auto before = calls;
        auto result = processor.process(u);
        require(calls == before && !result.wakePrefixRemoved && !result.error.empty(), "Uncertain prefix reached command handler");
    }
    recognizer.text = "амина включи свет амина на кухне";
    processor.process(u); require(received == "включи свет амина на кухне", "Middle wake word removed");
    u.inlineWake = false;
    for (const auto* text : {"амина включи свет на кухне", "афина включи свет на кухне", "включи свет амина на кухне"}) {
        recognizer.text = text; auto result = processor.process(u);
        require(received == text && !result.wakePrefixRemoved, "Non-inline transcript changed");
    }
}
void scenario(const std::string& mode, const std::string& text = "амина включи свет на кухне", int brightness = 100) {
    Vad vad; Wake wake; Ack ack; Source source; VoiceSegmenter segmenter(vad,{0,0,15000});
    // KWS admits 256 ms after command onset; prefix and first 8 command frames
    // must come from the pre-detection ring, byte-for-byte, not a fake transcript.
    wake.trigger = mode == "no-wake" ? 0 : mode == "wrapped" ? 140 : 40;
    Clock::time_point time{}, detected{}, decision{};
    std::string phase; int reads = 0, waits = 0, emitted = 0; bool stop = false;
    std::vector<float> observed;
    std::atomic<int> processed{0};
    Recognizer recognizer; recognizer.text = text; Model model;
    const auto& fixture = test::fixture();
    CommandEngine engine(model,fixture.parser,fixture.domain,[&](std::string_view raw) {
        return llm::CompactActionDecoder(fixture.domain, {"device_kitchen_light","action_set_brightness"}).decode(raw);
    });
    VoiceProcessor processor(recognizer,engine);
    source.next = [&] {
        // Once the 256 ms grace is captured, pace pending probe frames at
        // their actual duration. Pumping 32 ms of virtual audio per 1 ms
        // future wait makes thread scheduling consume the fake latency budget.
        if (phase == "CHECKING_CONTINUATION" && time - detected >= std::chrono::milliseconds(256))
            std::this_thread::sleep_for(std::chrono::milliseconds(32));
        time += std::chrono::milliseconds(32); ++reads;
        require(reads < 200000, "Loop stalled");
        if ((mode == "stop-check" && phase == "CHECKING_CONTINUATION") ||
            (mode == "stop-inline" && phase == "CAPTURING_INLINE_COMMAND")) stop = true;
        if ((waits >= 2 && reads > 120) || (mode == "no-wake" && reads > 120)) stop = true;
        if (phase == "BEEP_START" || phase == "BEEP_END") return .9f;
        if (mode == "separated" || mode == "pause" || mode == "timeout") {
            if (reads >= 20 && reads <= 40) return .1f;
            if (mode != "timeout" && (phase == "LISTENING_COMMAND" || phase == "CAPTURING_COMMAND") && reads < 100) return .3f;
            return 0.0f;
        }
        const int position = reads - (mode == "wrapped" ? 100 : 0);
        if (mode == "history" && position >= 2 && position <= 10) return .2f;
        if (position >= 20 && position <= 32) return .1f; // Wake word.
        if (position >= 33 && position <= 65) return .3f; // Continuous command.
        return 0.0f;
    };
    std::vector<std::string> states;
    runWakeVoiceLoop(source,wake,segmenter,{},[&](const Utterance& u) {
        ++processed; observed = u.audio;
        if (mode == "inline" || mode == "wrapped" || mode == "history") {
            require(u.inlineWake && u.audio.size() == 46 * FrameSamples, "Utterance lost pre-detection frames");
            require(std::count(u.audio.begin(),u.audio.end(),.1f) == 13 * FrameSamples, "Wake prefix PCM lost");
            require(std::count(u.audio.begin(),u.audio.end(),.3f) == 33 * FrameSamples, "Command onset PCM lost");
        }
        require(std::find(u.audio.begin(),u.audio.end(),.9f) == u.audio.end(), "Beep entered STT");
        return processor.process(u);
    },[&](const VoiceResult& result) {
        ++emitted;
        if (brightness >= 0) {
            const auto* action = std::get_if<actions::GenericAction>(&result.action);
            require(action && std::get<int64_t>(action->arguments().at("brightness").data()) == brightness, "Wrong action after wake");
            if (brightness == 40) require(result.route == "LLM_FALLBACK", "Compact fallback not exercised");
        } else require(std::holds_alternative<actions::Unsupported>(result.action), "Unsafe command executed");
    },[&](const std::string& value) {
        phase = value; states.push_back(value);
        if (value == "WAITING_WAKE") ++waits;
        if (value == "WAKE_DETECTED") detected = time;
        if (value == "CONTINUATION_INLINE" || value == "CONTINUATION_SEPARATED") decision = time;
    },[&] { return stop; },[&] { return time; },&ack);
    const bool separated = mode == "separated" || mode == "pause" || mode == "timeout";
    require(ack.starts == (separated ? 1 : 0) && ack.cancels > 0, "Wrong beep path/cleanup");
    if (mode.rfind("stop-",0) == 0 || mode == "no-wake" || mode == "timeout") require(processed == 0, "Unadmitted/stopped speech processed");
    else require(processed == 1 && emitted == 1 && waits == 2, "Not exactly one command per wake");
    if (mode != "no-wake" && mode != "stop-check") require(decision - detected >= std::chrono::milliseconds(256) && decision - detected <= std::chrono::milliseconds(512), "Decision outside bounded grace/probe handoff");
}
void shutdownProbe() {
    struct BlockingVad : SpeechDetector {
        std::atomic<bool> entered{false}, release{false}, finished{false};
        Detection accept(const float*, int) override {
            entered = true;
            while (!release) std::this_thread::yield();
            finished = true; return {};
        }
        void reset() override {}
    } vad;
    Source source; Wake wake; wake.trigger=1; Ack ack; VoiceSegmenter segmenter(vad);
    bool stop=false; int reads=0; std::string last;
    const auto owner=std::this_thread::get_id();
    source.next=[&] {
        require(std::this_thread::get_id()==owner,"Second capture consumer");
        require(++reads<1000,"Probe blocked capture");
        if(vad.entered) { stop=true;vad.release=true; }
        return 0.0f;
    };
    runWakeVoiceLoop(source,wake,segmenter,{},[](const Utterance&) -> VoiceResult { throw std::runtime_error("Stopped probe executed"); },
        [](const VoiceResult&) {},[&](const std::string& value) {last=value;},[&] {return stop;},Clock::now,&ack);
    require(vad.entered && vad.finished && last=="stopped" && ack.starts==0,"Probe worker not joined");
}
void probeOverflow() {
    struct Blocked : SpeechDetector {
        std::atomic<bool> release{false};
        Detection accept(const float*,int) override {
            while(!release)std::this_thread::yield();
            return {};
        }
        void reset() override {}
    } vad;
    Source source; Wake wake; wake.trigger=1; VoiceSegmenter segmenter(vad);
    int reads=0; source.next=[&] { require(++reads<1000,"Unbounded pending audio");return 0.0f; };
    bool rejected=false;
    try {
        runWakeVoiceLoop(source,wake,segmenter,{},[](const Utterance&) -> VoiceResult { throw std::runtime_error("Overflow executed"); },
            [](const VoiceResult&) {},[&](const std::string& value) {if(value=="WAKE_UNAVAILABLE")vad.release=true;},[] {return false;});
    } catch(const std::exception& e) {
        rejected=std::string(e.what())=="Continuation probe exceeded bounded audio history";
    }
    require(rejected && vad.release,"Pending probe overflow did not fail closed/join");
}
int main() { try {
    WakePcmRing ring; std::vector<float> pcm(100000);
    for (size_t i=0;i<pcm.size();++i) pcm[i]=static_cast<float>(i);
    ring.append(pcm.data(),pcm.size()); auto saved=ring.snapshot();
    require(saved.size()==48000 && saved.front()==52000 && saved.back()==99999,"Ring capacity/order incorrect");
    ring.clear(); require(ring.size()==0 && ring.snapshot().empty(),"Previous session history retained");
    prefixTests();
    shutdownProbe();
    probeOverflow();
    scenario("inline");
    scenario("wrapped");
    scenario("history");
    scenario("inline","афина выключи свет на кухне",0);
    scenario("inline","Амина поставь свет на кухне на сорок процентов",40);
    scenario("inline","амина свет на кухне",-1);
    scenario("inline","амина на кухне выключи свет на 40 процентов",-1);
    scenario("inline","включи свет на кухне",-1);
    scenario("separated","включи свет на кухне");
    scenario("pause","неподдерживаемая речь",-1);
    for (const auto* mode : {"timeout","no-wake","stop-check","stop-inline"}) scenario(mode);
    std::cout << "Seamless PCM recovery, prefix scope, compact/safety, beep and shutdown passed\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
