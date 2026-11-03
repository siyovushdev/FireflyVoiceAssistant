#include "firefly/voice/WakeVoiceLoop.h"
#include <array>
#include "firefly/voice/WakePcmRing.h"
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
    WakeAcknowledgement* acknowledgement,
    const std::function<bool(const std::vector<float>&)>& confirmWake) {
    config.validate();
    if (!config.enabled) throw std::invalid_argument("Wake loop requires enabled wake gating");
    if (config.afterBeepOnly && !acknowledgement)
        throw std::invalid_argument("After-beep mode requires wake acknowledgement");
    if (confirmWake && !config.afterBeepOnly)
        throw std::invalid_argument("STT wake confirmation requires after-beep mode");
    struct Cleanup {
        WakeAcknowledgement* output;
        ~Cleanup() { if (output) output->cancel(); }
    } cleanup{acknowledgement};
    enum class Phase { WaitingWake, ConfirmingTail, VerifyingWake, CheckingContinuation, DecidingContinuation, Acknowledging, Draining, WaitingSpeech, Capturing, CapturingInline, Processing };
    Phase phase = Phase::WaitingWake;
    std::future<VoiceResult> worker;
    std::future<bool> continuationWorker;
    std::future<bool> confirmationWorker;
    auto confirmationDeadline = now();
    bool confirmationExpired = false;
    std::vector<float> confirmationPcm;
    std::size_t confirmationHistory = 0;
    constexpr std::size_t ConfirmationTailSamples = 8 * FrameSamples; // 256 ms, actual capture only
    std::array<float, FrameSamples> frame{};
    int filled = 0;
    WakePcmRing ring;
    std::vector<float> continuation;
    constexpr int GraceSamples = SampleRate * 256 / 1000;
    // Capture continues while the exclusive VAD probe runs. Bound even a slow
    // backend to one second of post-detection PCM; failure never opens gating.
    constexpr int MaxContinuationSamples = SampleRate;
    continuation.reserve(MaxContinuationSamples);
    int64_t wakeOffset = 0;
    auto deadline = now(), legacyDeadline = now(), captureDeadline = now();
    auto lastAudio = now();
    auto rearm = [&] {
        wake.reset(); segmenter.startSession(); filled = 0; ring.clear(); continuation.clear(); confirmationPcm.clear();
        phase = Phase::WaitingWake; state("WAITING_WAKE");
    };
    auto arm = [&] {
        // The wake frame, playback PCM and buffered echo must never reach STT.
        segmenter.startSession(); filled = 0;
        deadline = now() + config.commandStartTimeout + std::chrono::milliseconds(segmenter.startConfirmationMs());
        legacyDeadline = now() + config.commandTimeout;
        phase = Phase::WaitingSpeech; state("LISTENING_COMMAND");
    };
    auto submit = [&](Utterance utterance, bool inlineWake) {
        utterance.inlineWake = inlineWake;
        utterance.wakeAdmitted = true;
        if (!utterance.error.empty()) {
            state("COMMAND_MAX_DURATION");
            VoiceResult rejected; rejected.inlineWake = inlineWake; rejected.error = utterance.error; emit(rejected); rearm();
        } else {
            state("SPEECH_END"); phase = Phase::Processing; state("PROCESSING");
            if (!stopped()) worker = std::async(std::launch::async,
                [&process, u = std::move(utterance)] { return process(u); });
        }
    };
    auto finishContinuation = [&](bool inlineWake) {
        state(inlineWake ? "CONTINUATION_INLINE" : "CONTINUATION_SEPARATED");
        if (stopped()) return;
        if (inlineWake) {
            auto buffered = ring.snapshot();
            buffered.insert(buffered.end(), continuation.begin(), continuation.end());
            ring.clear(); continuation.clear(); segmenter.startSession();
            phase = Phase::CapturingInline; state("SPEECH_START"); state("CAPTURING_INLINE_COMMAND");
            captureDeadline = now() + std::chrono::milliseconds(segmenter.maxSpeechMs() + 2000);
            legacyDeadline = now() + config.commandTimeout;
            // One authoritative utterance. Earlier completed speech in the
            // ring is never queued or dispatched as a command.
            for (size_t offset = 0; offset < buffered.size() && !stopped(); offset += FrameSamples) {
                auto ready = segmenter.accept(buffered.data() + offset,
                    std::min<size_t>(FrameSamples, buffered.size() - offset));
                for (auto& u : ready) {
                    if (!u.error.empty() || u.speech.end >= wakeOffset) {
                        submit(std::move(u), true); break;
                    }
                }
                if (phase != Phase::CapturingInline) break;
            }
        } else {
            ring.clear(); continuation.clear(); segmenter.startSession();
            state("ACKNOWLEDGING_WAKE");
            if (acknowledgement) {
                phase = Phase::Acknowledging;
                deadline = now() + std::chrono::seconds(1);
                state("BEEP_START"); acknowledgement->start();
            } else arm();
        }
    };
    try {
        rearm();
        while (!stopped()) {
            // Capture drains while the idle STT is used exclusively for wake
            // verification. No queued candidate, command, VAD or playback here.
            if (confirmationWorker.valid()) {
                if (!confirmationExpired && now() >= confirmationDeadline) {
                    confirmationExpired = true; state("WAKE_CONFIRMATION_TIMEOUT");
                }
                if (confirmationWorker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    bool accepted = false;
                    try { accepted = confirmationWorker.get(); }
                    catch (...) { state("WAKE_CONFIRMATION_FAILED"); }
                    if (stopped()) break;
                    if (accepted && !confirmationExpired && now() < confirmationDeadline) {
                        state("WAKE_CONFIRMED"); finishContinuation(false);
                    } else { state("WAKE_REJECTED"); rearm(); }
                    if (stopped()) break;
                }
            }
            // At most one VAD owner: capture never touches it until get() joins
            // the probe. A 1 ms bounded wait avoids spinning on synthetic sources.
            if (continuationWorker.valid() &&
                continuationWorker.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
                finishContinuation(continuationWorker.get());
                if (stopped()) break;
            }
            if (worker.valid() && worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto result = worker.get();
                if (stopped()) break;
                emit(result);
                if (config.afterBeepOnly && result.wakeOnly) {
                    state("WAKE_REPEATED");
                    finishContinuation(false);
                } else rearm();
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
                    if (status == AcknowledgementStatus::Failed && config.afterBeepOnly) {
                        acknowledgement->cancel(); rearm(); continue;
                    }
                    phase = Phase::Draining; deadline = now() + config.acknowledgementDrain;
                }
                continue;
            }
            if (phase == Phase::Draining) {
                if (now() >= deadline) arm();
                continue;
            }
            if (phase == Phase::WaitingWake) {
                ring.append(frame.data(), FrameSamples);
                if (wake.accept(frame.data(), FrameSamples)) {
                    state("WAKE_DETECTED");
                    wakeOffset = static_cast<int64_t>(ring.size());
                    if (confirmWake) {
                        // Finish the name from the same capture stream. This
                        // bounded real tail is never VAD/command input or padding.
                        confirmationPcm = ring.snapshot(); confirmationHistory = confirmationPcm.size();
                        phase = Phase::ConfirmingTail; state("VERIFYING_WAKE");
                        confirmationExpired = false; confirmationDeadline = now() + std::chrono::seconds(2);
                    } else if (config.afterBeepOnly) {
                        // Neither room speech nor a repeated wake can silently
                        // choose inline capture. Pre-beep PCM is never a command.
                        finishContinuation(false);
                    } else {
                        segmenter.startSession(); continuation.clear();
                        phase = Phase::CheckingContinuation; state("CHECKING_CONTINUATION");
                    }
                }
            } else if (phase == Phase::ConfirmingTail) {
                if (now() >= confirmationDeadline) {
                    state("WAKE_CONFIRMATION_TIMEOUT"); state("WAKE_REJECTED"); rearm(); continue;
                }
                confirmationPcm.insert(confirmationPcm.end(), frame.begin(), frame.end());
                if (confirmationPcm.size() - confirmationHistory >= ConfirmationTailSamples) {
                    phase = Phase::VerifyingWake;
                    confirmationWorker = std::async(std::launch::async,
                        [&confirmWake, pcm = std::move(confirmationPcm)] { return confirmWake(pcm); });
                }
            } else if (phase == Phase::CheckingContinuation) {
                continuation.insert(continuation.end(), frame.begin(), frame.end());
                if (continuation.size() < GraceSamples) continue;
                auto buffered = ring.snapshot();
                buffered.insert(buffered.end(), continuation.begin(), continuation.end());
                phase = Phase::DecidingContinuation;
                continuationWorker = std::async(std::launch::async,
                    [&segmenter, pcm = std::move(buffered), offset = wakeOffset] {
                        return segmenter.continuesAfterWake(pcm, offset);
                    });
            } else if (phase == Phase::DecidingContinuation) {
                if (continuation.size() + FrameSamples > MaxContinuationSamples)
                    throw std::runtime_error("Continuation probe exceeded bounded audio history");
                continuation.insert(continuation.end(), frame.begin(), frame.end());
            } else if (phase == Phase::WaitingSpeech || phase == Phase::Capturing || phase == Phase::CapturingInline) {
                if (config.commandTimeout.count() && now() >= legacyDeadline) {
                    state("COMMAND_TIMEOUT"); rearm(); continue;
                }
                if (phase == Phase::WaitingSpeech && now() >= deadline) {
                    state("COMMAND_START_TIMEOUT"); rearm(); continue;
                }
                if (phase != Phase::WaitingSpeech && now() >= captureDeadline) {
                    state("COMMAND_MAX_DURATION"); rearm(); continue;
                }
                auto utterances = segmenter.accept(frame.data(), FrameSamples);
                if (phase == Phase::WaitingSpeech && segmenter.speechSeen()) {
                    phase = Phase::Capturing;
                    // Independent hard bound includes endpoint/post-roll. The segmenter
                    // additionally enforces the exact configured maximum speech length.
                    captureDeadline = now() + std::chrono::milliseconds(segmenter.maxSpeechMs() + 2000);
                    state("SPEECH_START"); state("CAPTURING_COMMAND");
                }
                if (utterances.empty()) continue;
                for (auto& utterance : utterances) {
                    if (phase == Phase::CapturingInline && utterance.error.empty() && utterance.speech.end < wakeOffset) continue;
                    submit(std::move(utterance), phase == Phase::CapturingInline); break;
                }
            }
        }
        if (acknowledgement) acknowledgement->cancel();
        state("stopping");
        if (continuationWorker.valid()) continuationWorker.get();
        if (confirmationWorker.valid()) confirmationWorker.wait();
        if (worker.valid()) worker.get();
        state("stopped");
    } catch (...) {
        // Future destruction also joins on exception. Never resume ungated capture.
        if (acknowledgement) acknowledgement->cancel();
        state("WAKE_UNAVAILABLE");
        if (continuationWorker.valid()) continuationWorker.wait();
        if (confirmationWorker.valid()) confirmationWorker.wait();
        if (worker.valid()) worker.wait();
        throw;
    }
}
}
