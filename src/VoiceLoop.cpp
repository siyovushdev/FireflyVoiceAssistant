#include "firefly/voice/VoiceLoop.h"
#include "firefly/voice/AlsaAudioSource.h"
#include <array>
#include <chrono>
#include <future>
namespace firefly::voice {
void runVoiceLoop(AudioSource& capture, VoiceSegmenter& segmenter,
    const std::function<VoiceResult(const Utterance&)>& process,
    const std::function<void(const VoiceResult&)>& emit,
    const std::function<void(const std::string&)>& state,
    const std::function<bool()>& stopped) {
    std::future<VoiceResult> worker;
    state("listening");
    std::array<float, FrameSamples> frame{};
    int filled = 0;
    bool waitingQuiet = false;
    while (!stopped()) {
        if (worker.valid() && worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = worker.get();
            if (stopped()) break;
            emit(result);
            // Audio spoken during PROCESS is never queued for later execution. Require
            // a fresh quiet interval before rearming, including a tail still in progress.
            segmenter.discontinuity(); filled = 0; waitingQuiet = true;
            state("waiting_for_quiet");
        }
        int n;
        try { n = capture.read(frame.data() + filled, FrameSamples - filled); }
        catch (const AudioDiscontinuity& e) {
            segmenter.discontinuity(); filled = 0; waitingQuiet = true;
            VoiceResult rejected; rejected.error = e.what(); emit(rejected); continue;
        }
        filled += n;
        if (filled != FrameSamples) continue;
        auto utterances = segmenter.accept(frame.data(), filled); filled = 0;
        if (waitingQuiet && !segmenter.active() && !worker.valid()) {
            waitingQuiet = false; state("listening");
        }
        for (auto& u : utterances) {
            if (worker.valid()) {
                VoiceResult rejected; rejected.error = "Speech while PROCESS busy was discarded"; emit(rejected);
            } else if (!u.error.empty()) {
                waitingQuiet = true;
                VoiceResult rejected; rejected.error = u.error; emit(rejected);
            } else {
                state("processing");
                worker = std::async(std::launch::async, [&process, u = std::move(u)] { return process(u); });
            }
        }
    }
    state("stopping");
    // No detached vendor call and no action emitted after a stop request.
    if (worker.valid()) worker.get();
    state("stopped");
}
}
