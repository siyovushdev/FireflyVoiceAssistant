#pragma once
// Test-only file driver. No new HTTP API, ALSA capture, STT/NPU model or device.
// Synthetic PCM/KWS/VAD/STT feed the real wake loop and RuntimeWorker::processVoice.
#include "firefly/voice/WakeVoiceLoop.h"
#include <algorithm>
#include <fstream>

namespace m19_test {
using Json = admin_test::Json;
using Clock = std::chrono::steady_clock;

class VoiceDriver {
public:
    explicit VoiceDriver(firefly::admin::RuntimeWorker& worker) : worker_(worker) {
        const auto* path = std::getenv("FIREFLY_M19_VOICE_CONTROL");
        if (!path) throw std::runtime_error("M19 fixture requires private control file");
        path_ = path;
        worker_.voiceStatus("WAITING_WAKE", "fixture");
    }

    void poll() {
        if (!std::filesystem::exists(path_)) return;
        std::ifstream input(path_);
        const auto request = Json::parse(input);
        const auto id = request.at("id").get<int>();
        if (id == lastId_) return;
        // Mark consumed before processing: a failure never replays an utterance.
        lastId_ = id;
        std::filesystem::remove(path_);
        Json states = Json::array();
        int processed = 0, emitted = 0, reads = 0, waits = 0;
        std::string phase;
        Clock::time_point time{}, detected{};
        struct Vad final : firefly::voice::SpeechDetector {
            int64_t received = 0, start = -1, end = 0;
            firefly::voice::Detection accept(const float* pcm, int count) override {
                const auto previous = received; received += count;
                if (pcm[0] > 0) {
                    if (start < 0) start = previous;
                    end = received;
                    return {end - start >= 4096, {}};
                }
                if (start >= 0) {
                    const auto begin = start; start = -1;
                    return {false, {{begin, end}}};
                }
                return {};
            }
            void reset() override { received = end = 0; start = -1; }
        } vad;
        struct Wake final : firefly::voice::WakeDetector {
            int calls = 0;
            bool enabled = true;
            bool accept(const float*, int) override { return ++calls == 40 && enabled; }
            void reset() override {}
        } wake;
        wake.enabled = request.value("wake", true);
        struct Recognizer final : firefly::voice::SpeechRecognizer {
            std::string text;
            firefly::voice::RecognitionResult recognize(const std::vector<float>&) override {
                return {true, text, {}, 0};
            }
        } recognizer;
        recognizer.text = request.at("text").get<std::string>();
        struct Source final : firefly::voice::AudioSource {
            std::function<float()> next;
            int read(float* pcm, int count) override {
                std::fill_n(pcm, count, next());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return count;
            }
        } source;
        source.next = [&] {
            if (phase == "CHECKING_CONTINUATION" && time - detected >= std::chrono::milliseconds(256))
                std::this_thread::sleep_for(std::chrono::milliseconds(32));
            time += std::chrono::milliseconds(32);
            if (++reads > 10000) throw std::runtime_error("M19 fixture wake loop stalled");
            return reads >= 20 && reads <= 65 ? .3f : 0.0f;
        };
        firefly::voice::VoiceSegmenter segmenter(vad, {0, 0, 15000});
        firefly::voice::runWakeVoiceLoop(source, wake, segmenter, {},
            [&](const firefly::voice::Utterance& utterance) {
                ++processed;
                return worker_.processVoice(recognizer, utterance);
            },
            [&](const firefly::voice::VoiceResult&) { ++emitted; },
            [&](const std::string& state) {
                phase = state;
                states.push_back(state);
                worker_.voiceStatus(state, "fixture");
                if (state == "WAITING_WAKE") ++waits;
                if (state == "WAKE_DETECTED") detected = time;
            },
            [&] { return (waits >= 2 && reads > 120) || (!wake.enabled && reads > 120); },
            [&] { return time; });
        worker_.voiceStatus("WAITING_WAKE", "fixture");
        const auto staged = path_ + ".result.tmp";
        { std::ofstream output(staged); output << Json{{"id", id}, {"processed", processed},
            {"emitted", emitted}, {"states", states}, {"runtime", Json::parse(worker_.observe())}}.dump(2); }
        std::filesystem::rename(staged, path_ + ".result");
    }

private:
    firefly::admin::RuntimeWorker& worker_;
    std::string path_;
    int lastId_ = -1;
};
}
