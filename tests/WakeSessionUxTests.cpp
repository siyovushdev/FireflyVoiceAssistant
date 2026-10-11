#include "firefly/voice/WakeVoiceLoop.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace firefly::voice;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Wake : WakeDetector {
    int calls = 0;
    bool accept(const float* data, int) override { ++calls; return data[0] == 1; }
    void reset() override {}
};
struct Detector : SpeechDetector {
    bool delayed = false;
    int startConfirmationMs() const override { return delayed ? 288 : 0; }
    int samples = 0, start = -1, end = 0, calls = 0;
    Detection accept(const float* data, int n) override {
        ++calls; require(data[0] != .9f, "Beep PCM entered VAD");
        const auto previous = samples; samples += n;
        if (data[0] == .3f) { if (start < 0) start = previous; end = samples; return {!delayed || samples - start >= 4000,{}}; }
        if (start >= 0) { const auto begin = start; start = -1; return {false,{{begin,end}}}; }
        return {};
    }
    void reset() override { samples = 0; start = -1; }
};
struct Ack : WakeAcknowledgement {
    int starts = 0, polls = 0, cancels = 0;
    bool fail = false, absent = false, stalled = false;
    void start() override { ++starts; }
    AcknowledgementStatus poll() override {
        ++polls;
        if (absent) return AcknowledgementStatus::Failed;
        if (stalled || polls < 13) return AcknowledgementStatus::Playing; // 416 ms at 32 ms/frame.
        return fail ? AcknowledgementStatus::Failed : AcknowledgementStatus::Complete;
    }
    void cancel() noexcept override { ++cancels; }
};
struct Source : AudioSource {
    std::function<float()> next;
    int read(float* data, int n) override { std::fill(data,data+n,next()); std::this_thread::yield(); return n; }
};
void scenario(const std::string& mode) {
    Wake wake; Detector detector; Ack ack; Source source;
    detector.delayed = mode == "confirmation-delay";
    ack.fail = mode == "beep-failure";
    ack.absent = mode == "beep-absent";
    ack.stalled = mode == "beep-stalled";
    const bool enabled = mode != "beep-disabled";
    VoiceSegmenter segmenter(detector, {300,300,1000});
    WakeConfig config;
    Clock::time_point time{}, armed{}, captured{}, beepStarted{}, beepEnded{};
    std::string phase;
    std::vector<std::string> states;
    int reads = 0, waits = 0;
    bool stop = false;
    std::atomic<int> processed{0};
    int emitted = 0;
    source.next = [&] {
        time += std::chrono::milliseconds(32);
        if (++reads > 100000) throw std::runtime_error("Loop failed to finish");
        if ((mode == "stop-beep" && phase == "BEEP_START") ||
            (mode == "stop-drain" && phase == "BEEP_END") ||
            (mode == "stop-wait" && phase == "LISTENING_COMMAND") ||
            (mode == "stop-capture" && phase == "CAPTURING_COMMAND")) stop = true;
        if (waits >= 2 && reads > 500) stop = true;
        if (reads == 1) return 1.0f;
        if (phase == "BEEP_START" || phase == "BEEP_END" || phase == "ACKNOWLEDGEMENT_FAILED") return .9f;
        if (phase == "LISTENING_COMMAND" || phase == "CAPTURING_COMMAND") {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time - armed).count();
            if (mode != "timeout" && ms >= 4800 && (mode == "long" || ms < 5500)) return .3f;
        }
        return 0.0f; // Includes ordinary speech after a completed session: no new wake.
    };
    runWakeVoiceLoop(source, wake, segmenter, config,
        [&](const Utterance& u) {
            ++processed;
            require(std::find(u.audio.begin(),u.audio.end(),.9f)==u.audio.end(), "Beep PCM reached STT");
            VoiceResult result; if (mode == "unsupported") result.error = "Unsupported";
            return result;
        }, [&](const VoiceResult&) { ++emitted; },
        [&](const std::string& state) {
            phase = state; states.push_back(state);
            if (state == "WAITING_WAKE") ++waits;
            if (state == "BEEP_START") beepStarted = time;
            if (state == "BEEP_END" || state == "ACKNOWLEDGEMENT_FAILED") beepEnded = time;
            if (state == "LISTENING_COMMAND") {
                armed = time;
                require(detector.samples == 0, "VAD/pre-roll not cleared before admission");
                if (enabled) {
                    require(ack.polls == (ack.absent ? 1 : ack.stalled ? 32 : 13), "Wrong playback duration/deadline");
                    require(time - beepEnded >= config.acknowledgementDrain, "Echo drain missing");
                    if (!ack.absent && !ack.stalled)
                        require(beepEnded - beepStarted == std::chrono::milliseconds(416), "Longer beep not exercised");
                } else require(ack.starts == 0 && ack.polls == 0, "Disabled beep used playback");
            }
            if (state == "CAPTURING_COMMAND") captured = time;
            if (state == "COMMAND_START_TIMEOUT") {
                require(time - armed >= config.commandStartTimeout, "Early start timeout");
                require(time - armed <= config.commandStartTimeout +
                    std::chrono::milliseconds(segmenter.startConfirmationMs() + 32), "Late start timeout");
            }
        }, [&] { return stop; }, [&] { return time; }, enabled ? &ack : nullptr);
    require(ack.starts == (enabled ? 1 : 0) && (!enabled || ack.cancels > 0) && states.back() == "stopped", "Acknowledgement cleanup failed");
    if (!enabled) require(std::find(states.begin(), states.end(), "BEEP_START") == states.end(), "Disabled beep logged playback");
    if (mode.rfind("stop-",0)==0 || mode == "timeout" || mode == "long") require(processed == 0, "Rejected/stopped speech processed");
    else {
        require(processed == 1 && emitted == 1 && waits == 2, "Late speech cut off or repeated session admitted");
        require(captured - armed >= std::chrono::milliseconds(4800), "Test did not exercise near-deadline speech");
    }
    if (mode == "long") require(std::find(states.begin(),states.end(),"COMMAND_MAX_DURATION")!=states.end(),"Unbounded speech");
    if (mode == "timeout") require(std::find(states.begin(),states.end(),"COMMAND_START_TIMEOUT")!=states.end(),"No-speech timeout missing");
    if (ack.fail || ack.absent || ack.stalled)
        require(std::find(states.begin(),states.end(),"ACKNOWLEDGEMENT_FAILED")!=states.end(),"Playback failure not observable");
}
int main() { try {
    for (const auto* mode : {"success","confirmation-delay","unsupported","timeout","long","beep-failure",
        "beep-absent","beep-stalled","beep-disabled","stop-beep","stop-drain","stop-wait","stop-capture"}) scenario(mode);
    std::cout << "13 wake UX cases passed: 416 ms beep, echo/pre-roll isolation, timeout, disabled/failure and shutdown\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
