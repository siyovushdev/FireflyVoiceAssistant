#include "firefly/voice/WakeVoiceLoop.h"
#include <array>
#include <future>
#include <stdexcept>
namespace firefly::voice {
void WakeConfig::validate() const {
    if (cutoff != 0.99 || window != 7)
        throw std::invalid_argument("Amina V1 requires frozen cutoff 0.99 and window 7");
    if (commandTimeout < std::chrono::milliseconds(1000) || commandTimeout > std::chrono::milliseconds(60000))
        throw std::invalid_argument("Command listen timeout must be 1000..60000 ms");
}
void runWakeVoiceLoop(AudioSource& capture, WakeDetector& wake, VoiceSegmenter& segmenter,
    const WakeConfig& config,
    const std::function<VoiceResult(const Utterance&)>& process,
    const std::function<void(const VoiceResult&)>& emit,
    const std::function<void(const std::string&)>& state,
    const std::function<bool()>& stopped,
    const std::function<std::chrono::steady_clock::time_point()>& now) {
    config.validate();
    if (!config.enabled) throw std::invalid_argument("Wake loop requires enabled wake gating");
    enum class Phase { WaitingWake, ListeningCommand, Processing };
    Phase phase = Phase::WaitingWake;
    std::future<VoiceResult> worker;
    std::array<float, FrameSamples> frame{};
    int filled = 0;
    auto deadline = now();
    auto lastAudio = now();
    auto rearm = [&] {
        wake.reset();
        segmenter.startSession();
        filled = 0;
        phase = Phase::WaitingWake;
        state("WAITING_WAKE");
    };
    try {
        rearm();
        while (!stopped()) {
            if (worker.valid() && worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto result = worker.get();
                if (stopped()) break;
                emit(result);
                rearm();
            }
            if (phase == Phase::ListeningCommand && now() >= deadline) {
                state("COMMAND_TIMEOUT");
                rearm();
            }
            const int n = capture.read(frame.data() + filled, FrameSamples - filled);
            if (stopped()) break;
            if (n < 0 || n > FrameSamples - filled) throw std::runtime_error("Invalid audio read count");
            if (n > 0) lastAudio = now();
            else if (now() - lastAudio >= std::chrono::seconds(2))
                throw std::runtime_error("Wake audio stalled for 2 seconds");
            filled += n;
            if (filled != FrameSamples) continue;
            filled = 0;
            if (phase == Phase::WaitingWake) {
                if (wake.accept(frame.data(), FrameSamples)) {
                    state("WAKE_DETECTED"); // V1 acknowledgement; no audio-output subsystem.
                    segmenter.startSession(); // No pre-wake PCM can reach STT.
                    deadline = now() + config.commandTimeout;
                    phase = Phase::ListeningCommand;
                    state("LISTENING_COMMAND");
                }
            } else if (phase == Phase::ListeningCommand) {
                // Check again after bounded capture wait: expired audio never reaches STT.
                if (now() >= deadline) { state("COMMAND_TIMEOUT"); rearm(); continue; }
                auto utterances = segmenter.accept(frame.data(), FrameSamples);
                if (utterances.empty()) continue;
                auto utterance = std::move(utterances.front());
                if (!utterance.error.empty()) {
                    VoiceResult rejected; rejected.error = utterance.error; emit(rejected);
                    rearm();
                } else {
                    phase = Phase::Processing;
                    state("PROCESSING");
                    if (stopped()) break;
                    worker = std::async(std::launch::async, [&process, u = std::move(utterance)] {
                        return process(u);
                    });
                }
            }
        }
        state("stopping");
        if (worker.valid()) worker.get();
        state("stopped");
    } catch (...) {
        // Future destruction also joins on exception. Never resume ungated capture.
        state("WAKE_UNAVAILABLE");
        if (worker.valid()) worker.wait();
        throw;
    }
}
}
