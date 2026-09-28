#include "firefly/voice/WakeVoiceLoop.h"
#include <array>
#include <future>
#include <stdexcept>
namespace firefly::voice {
void WakeConfig::validate() const {
    if (cutoff != 0.99 || window != 7)
        throw std::invalid_argument("Amina V1 requires frozen cutoff 0.99 and window 7");
    if (commandTimeout.count() != 0 &&
        (commandTimeout < std::chrono::milliseconds(1000) || commandTimeout > std::chrono::milliseconds(60000)))
        throw std::invalid_argument("Legacy command listen timeout must be 1000..60000 ms");
    if (commandStartTimeout < std::chrono::milliseconds(1000) || commandStartTimeout > std::chrono::milliseconds(15000))
        throw std::invalid_argument("Command start timeout must be 1000..15000 ms");
    if (acknowledgementDrain.count() < 0 || acknowledgementDrain > std::chrono::milliseconds(1000))
        throw std::invalid_argument("Invalid acknowledgement drain time");
}
void runWakeVoiceLoop(AudioSource& capture, WakeDetector& wake, VoiceSegmenter& segmenter,
    const WakeConfig& config,
    const std::function<VoiceResult(const Utterance&)>& process,
    const std::function<void(const VoiceResult&)>& emit,
    const std::function<void(const std::string&)>& state,
    const std::function<bool()>& stopped,
    const std::function<std::chrono::steady_clock::time_point()>& now,
    WakeAcknowledgement* acknowledgement) {
    config.validate();
    if (!config.enabled) throw std::invalid_argument("Wake loop requires enabled wake gating");
    struct Cleanup {
        WakeAcknowledgement* output;
        ~Cleanup() { if (output) output->cancel(); }
    } cleanup{acknowledgement};
    enum class Phase { WaitingWake, Acknowledging, Draining, WaitingSpeech, Capturing, Processing };
    Phase phase = Phase::WaitingWake;
    std::future<VoiceResult> worker;
    std::array<float, FrameSamples> frame{};
    int filled = 0;
    auto deadline = now(), legacyDeadline = now(), captureDeadline = now();
    auto lastAudio = now();
    auto rearm = [&] {
        wake.reset(); segmenter.startSession(); filled = 0;
        phase = Phase::WaitingWake; state("WAITING_WAKE");
    };
    auto arm = [&] {
        // The wake frame, playback PCM and buffered echo must never reach STT.
        segmenter.startSession(); filled = 0;
        deadline = now() + config.commandStartTimeout + std::chrono::milliseconds(segmenter.startConfirmationMs());
        legacyDeadline = now() + config.commandTimeout;
        phase = Phase::WaitingSpeech; state("LISTENING_COMMAND");
    };
    try {
        rearm();
        while (!stopped()) {
            if (worker.valid() && worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto result = worker.get();
                if (stopped()) break;
                emit(result); rearm();
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
            if (phase == Phase::Acknowledging) {
                auto status = acknowledgement->poll();
                if (now() >= deadline && status == AcknowledgementStatus::Playing) {
                    acknowledgement->cancel(); status = AcknowledgementStatus::Failed;
                }
                if (status != AcknowledgementStatus::Playing) {
                    state(status == AcknowledgementStatus::Failed ? "ACKNOWLEDGEMENT_FAILED" : "BEEP_END");
                    phase = Phase::Draining; deadline = now() + config.acknowledgementDrain;
                }
                continue;
            }
            if (phase == Phase::Draining) {
                if (now() >= deadline) arm();
                continue;
            }
            if (phase == Phase::WaitingWake) {
                if (wake.accept(frame.data(), FrameSamples)) {
                    state("WAKE_DETECTED");
                    state("ACKNOWLEDGING_WAKE");
                    if (acknowledgement) {
                        phase = Phase::Acknowledging;
                        deadline = now() + std::chrono::seconds(1);
                        state("BEEP_START");
                        acknowledgement->start();
                    } else arm(); // Injectable silent acknowledgement for non-playback callers.
                }
            } else if (phase == Phase::WaitingSpeech || phase == Phase::Capturing) {
                if (config.commandTimeout.count() && now() >= legacyDeadline) {
                    state("COMMAND_TIMEOUT"); rearm(); continue;
                }
                if (phase == Phase::WaitingSpeech && now() >= deadline) {
                    state("COMMAND_START_TIMEOUT"); rearm(); continue;
                }
                if (phase == Phase::Capturing && now() >= captureDeadline) {
                    state("COMMAND_MAX_DURATION"); rearm(); continue;
                }
                auto utterances = segmenter.accept(frame.data(), FrameSamples);
                if (phase == Phase::WaitingSpeech && segmenter.speechSeen()) {
                    phase = Phase::Capturing;
                    // Independent hard bound includes endpoint/post-roll. The segmenter
                    // additionally enforces the exact configured maximum speech length.
                    captureDeadline = now() + std::chrono::milliseconds(segmenter.maxSpeechMs() + 2000);
                    state("CAPTURING_COMMAND");
                }
                if (utterances.empty()) continue;
                auto utterance = std::move(utterances.front());
                if (!utterance.error.empty()) {
                    state("COMMAND_MAX_DURATION");
                    VoiceResult rejected; rejected.error = utterance.error; emit(rejected); rearm();
                } else {
                    state("SPEECH_END"); phase = Phase::Processing; state("PROCESSING");
                    if (stopped()) break;
                    worker = std::async(std::launch::async, [&process, u = std::move(utterance)] { return process(u); });
                }
            }
        }
        if (acknowledgement) acknowledgement->cancel();
        state("stopping");
        if (worker.valid()) worker.get();
        state("stopped");
    } catch (...) {
        // Future destruction also joins on exception. Never resume ungated capture.
        if (acknowledgement) acknowledgement->cancel();
        state("WAKE_UNAVAILABLE");
        if (worker.valid()) worker.wait();
        throw;
    }
}
}
