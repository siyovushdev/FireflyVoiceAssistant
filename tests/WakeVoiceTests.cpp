#include "firefly/voice/WakeVoiceLoop.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace firefly::voice;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Detector : SpeechDetector {
    int calls = 0, resets = 0;
    bool error = false;
    Detection accept(const float* pcm, int) override {
        ++calls;
        if (error) throw std::runtime_error("VAD failed");
        if (pcm[0] == 0) return {};
        // Two boundaries in the same block must still admit exactly one session.
        return {false, {{0, 128}, {128, 256}}};
    }
    void reset() override { ++resets; }
};
struct Wake : WakeDetector {
    int calls = 0, resets = 0;
    bool fail = false, failReset = false;
    bool accept(const float* samples, int) override {
        ++calls;
        if (fail) throw std::runtime_error("KWS failed");
        return samples[0] == 1;
    }
    void reset() override { ++resets; if (failReset) throw std::runtime_error("reset failed"); }
};
struct Source : AudioSource {
    int reads = 0;
    std::function<float(int)> sample;
    int read(float* samples, int count) override {
        const auto value = sample(++reads);
        std::fill(samples, samples + count, value);
        std::this_thread::yield();
        return count;
    }
};
void scenario(const std::string& outcome) {
    Source source; Wake wake; Detector detector; VoiceSegmenter segmenter(detector, {0,0,15000});
    bool stopped = false, processing = false, checking = false;
    int admissions = 0, emitted = 0, waits = 0, acks = 0;
    std::atomic<int> calls{0};
    std::vector<std::string> states;
    auto now = std::chrono::steady_clock::time_point{};
    WakeConfig config; config.commandTimeout = std::chrono::milliseconds(1000);
    source.sample = [&](int read) {
        now += std::chrono::milliseconds(32);
        if (read > 100000) throw std::runtime_error("Test failed to complete");
        if (outcome == "no-wake" && read > 50) stopped = true;
        if (waits >= 2 && read > 80) stopped = true;
        if (outcome == "timeout" && acks) now += std::chrono::seconds(2);
        if (outcome == "shutdown" && processing) stopped = true;
        if (checking) return 0.0f;
        // A repeated wake while busy is discarded, never queued as another session.
        return (read == 20 && outcome != "no-wake") || processing ? 1.0f : 0.25f;
    };
    runWakeVoiceLoop(source, wake, segmenter, config,
        [&](const Utterance& u) {
            ++calls;
            require(!u.audio.empty() && u.audio.front() == 0.25f, "Wake/pre-wake audio reached STT");
            VoiceResult result;
            if (outcome == "invalid") result.error = "Unknown/empty command";
            return result;
        }, [&](const VoiceResult&) { ++emitted; },
        [&](const std::string& state) {
            states.push_back(state);
            checking = state == "CHECKING_CONTINUATION";
            if (state == "WAKE_DETECTED") { ++acks; require(calls == 0, "Wake executed a command"); }
            if (state == "LISTENING_COMMAND") ++admissions;
            if (state == "WAITING_WAKE") { ++waits; processing = false; }
            if (state == "PROCESSING") processing = true;
        }, [&] { return stopped; }, [&] { return now; });
    require(states.back() == "stopped", "Shutdown missing");
    require(admissions == (outcome == "no-wake" ? 0 : 1), "Wake did not admit exactly one session");
    if (outcome == "no-wake" || outcome == "timeout") {
        require(calls == 0 && (outcome != "no-wake" || detector.calls == 0) && emitted == 0, "Speech without admitted session processed");
    } else if (outcome != "shutdown") {
        require(calls == 1 && emitted == 1 && waits == 2, "Command did not rearm or next speech processed");
    }
    if (outcome == "shutdown") require(emitted == 0, "Result emitted after stop");
}
void failure(const std::string& kind) {
    Source source; Wake wake; Detector detector; VoiceSegmenter segmenter(detector, {0,0,15000});
    wake.fail = kind == "wake"; wake.failReset = kind == "reset"; detector.error = kind == "vad";
    source.sample = [&](int read) {
        if (kind == "audio") throw std::runtime_error("capture failed");
        if (read > 20) throw std::runtime_error("Failure test overrun");
        return 1.0f;
    };
    int processed = 0; std::string state; bool threw = false;
    try {
        runWakeVoiceLoop(source, wake, segmenter, {}, [&](const Utterance&) { ++processed; return VoiceResult{}; },
            [](const VoiceResult&) {}, [&](const std::string& s) { state = s; }, [] { return false; });
    } catch (const std::exception& e) { threw = true; if (kind == "vad") require(std::string(e.what()) == "VAD failed", "VAD failure was not exercised"); }
    require(threw && state == "WAKE_UNAVAILABLE" && processed == 0, "Failure allowed ungated fallback");
}
void stalledAudio() {
    struct SilentSource : AudioSource { int read(float*, int) override { return 0; } } source;
    Wake wake; Detector detector; VoiceSegmenter segmenter(detector);
    auto time = std::chrono::steady_clock::time_point{};
    bool failed = false; std::string state;
    try {
        runWakeVoiceLoop(source, wake, segmenter, {},
            [](const Utterance&) -> VoiceResult { throw std::runtime_error("Stalled audio reached process"); },
            [](const VoiceResult&) {}, [&](const std::string& value) { state = value; }, [] { return false; },
            [&] { time += std::chrono::milliseconds(100); return time; });
    } catch (const std::exception& error) { failed = std::string(error.what()).find("stalled") != std::string::npos; }
    require(failed && state == "WAKE_UNAVAILABLE", "Audio stall reported healthy forever");
}
void joinedShutdown() {
    Source source; Wake wake; Detector detector; VoiceSegmenter segmenter(detector, {0,0,15000});
    std::atomic<bool> entered{false}, done{false}, stop{false};
    bool armed = false;
    source.sample = [&](int read) {
        if (entered) stop = true;
        return read == 1 ? 1.0f : armed ? 0.25f : 0.0f;
    };
    runWakeVoiceLoop(source, wake, segmenter, {}, [&](const Utterance&) {
        entered = true;
        while (!stop) std::this_thread::yield();
        done = true;
        return VoiceResult{};
    }, [](const VoiceResult&) { throw std::runtime_error("emit after shutdown"); },
    [&](const std::string& s) { if (s == "LISTENING_COMMAND") armed = true; }, [&] { return stop.load(); });
    require(entered && done, "Processing worker not joined");
}
}
int main() {
    try {
        for (const auto* outcome : {"no-wake", "success", "invalid", "timeout", "shutdown"}) scenario(outcome);
        for (const auto* kind : {"wake", "reset", "audio", "vad"}) failure(kind);
        joinedShutdown();
        stalledAudio();
        WakeConfig invalid; invalid.window = 6;
        bool threw = false; try { invalid.validate(); } catch (...) { threw = true; }
        require(threw, "Frozen window mutable");
        std::cout << "Wake state, admission, failure and shutdown tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
