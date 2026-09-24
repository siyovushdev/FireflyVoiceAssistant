#include "../admin/Support.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/admin/Api.h"
#include "firefly/voice/PcmAudio.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace admin_test;
namespace {
int checks = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
std::string wavMonoS16(int sampleRate, int frames, std::int16_t sample = 0) {
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames) * 2;
    std::string out(44 + dataBytes, '\0');
    auto* p = reinterpret_cast<unsigned char*>(&out[0]);
    auto put32 = [&](std::size_t o, std::uint32_t v) {
        p[o] = v & 0xff; p[o + 1] = (v >> 8) & 0xff; p[o + 2] = (v >> 16) & 0xff; p[o + 3] = (v >> 24) & 0xff;
    };
    auto put16 = [&](std::size_t o, std::uint16_t v) { p[o] = v & 0xff; p[o + 1] = (v >> 8) & 0xff; };
    std::memcpy(p, "RIFF", 4); put32(4, 36 + dataBytes); std::memcpy(p + 8, "WAVE", 4);
    std::memcpy(p + 12, "fmt ", 4); put32(16, 16); put16(20, 1); put16(22, 1); put32(24, sampleRate);
    put32(28, sampleRate * 2); put16(32, 2); put16(34, 16);
    std::memcpy(p + 36, "data", 4); put32(40, dataBytes);
    for (int i = 0; i < frames; ++i) put16(44 + i * 2, static_cast<std::uint16_t>(sample));
    return out;
}
class ScriptedRecognizer final : public firefly::voice::SpeechRecognizer {
public:
    firefly::voice::RecognitionResult next{true, "добавь телевизор в гостиную", "", 12.5};
    int calls = 0;
    firefly::voice::RecognitionResult recognize(const std::vector<float>& audio) override {
        ++calls;
        firefly::voice::RecognitionResult result = next;
        result.latencyMs = 12.5 + audio.size() * 0.0001;
        return result;
    }
};
class SlowRecognizer final : public firefly::voice::SpeechRecognizer {
public:
    std::atomic<bool> entered{false};
    firefly::voice::RecognitionResult recognize(const std::vector<float>&) override {
        entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return {true, "ok", "", 1};
    }
};
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto baseline = base(argv[1]);

        // WAV decode + resample
        const auto wav48 = wavMonoS16(48000, 48000); // 1 s
        const auto decoded = firefly::voice::decodeWavToSttPcm(wav48);
        check(decoded.sourceRate == 48000, "source rate retained");
        check(std::abs(decoded.durationMs - 1000.0) < 1.0, "duration ~1s");
        check(decoded.samples.size() == 16000, "resampled to 16 kHz");

        bool rejected = false;
        try { (void)firefly::voice::decodeWavToSttPcm("not-a-wav"); } catch (...) { rejected = true; }
        check(rejected, "malformed WAV rejected");

        rejected = false;
        try { (void)firefly::voice::decodeWavToSttPcm(wavMonoS16(48000, 48000 * 16)); } catch (...) { rejected = true; }
        check(rejected, "oversized duration rejected");

        // stereo: craft by patching channel count
        auto stereo = wavMonoS16(16000, 1600);
        stereo[22] = 2; stereo[23] = 0;
        rejected = false;
        try { (void)firefly::voice::decodeWavToSttPcm(stereo); } catch (...) { rejected = true; }
        check(rejected, "stereo rejected");

        auto badRate = wavMonoS16(16000, 1600);
        badRate[24] = 0x11; badRate[25] = 0x2b; // 11025
        rejected = false;
        try { (void)firefly::voice::decodeWavToSttPcm(badRate); } catch (...) { rejected = true; }
        check(rejected, "unsupported rate rejected");

        // Transcriber with scripted STT
        auto scripted = std::make_unique<ScriptedRecognizer>();
        auto* scriptedPtr = scripted.get();
        firefly::admin::AdminTranscriber transcriber(std::move(scripted));
        const auto ok = Json::parse(transcriber.transcribe(wavMonoS16(16000, 8000, 1000)));
        check(ok.at("ok") == true, "valid short WAV transcript ok");
        check(ok.at("text") == "добавь телевизор в гостиную", "transcript text");
        check(ok.at("copilot_started") == false, "does not start Copilot");
        check(ok.at("saved") == false && ok.at("applied") == false, "no save/apply");
        check(scriptedPtr->calls == 1, "STT called once");

        auto emptyRec = std::make_unique<ScriptedRecognizer>();
        emptyRec->next = firefly::voice::RecognitionResult{true, "  \n", "", 1};
        firefly::admin::AdminTranscriber emptyStt(std::move(emptyRec));
        const auto empty = Json::parse(emptyStt.transcribe(wavMonoS16(16000, 1600)));
        check(empty.at("ok") == false && empty.at("errors")[0].at("code") == "empty_speech", "empty speech rejected");

        const auto malformed = Json::parse(transcriber.transcribe("{"));
        check(malformed.at("ok") == false && malformed.at("errors")[0].at("code") == "invalid_audio", "malformed audio code");

        // API: unavailable without transcriber
        Directory directory;
        firefly::config::ConfigStore store(directory.path);
        store.initialize(catalog(baseline));
        class NullModel final : public Model {};
        NullModel model;
        firefly::admin::RuntimeWorker worker(model);
        firefly::admin::LocalRuntimeClient client(worker);
        firefly::admin::Api apiNoStt(store, client, nullptr);
        apiNoStt.initializeRuntime();
        auto missing = apiNoStt.handle("POST", "/api/admin/transcribe", wavMonoS16(16000, 1600));
        check(missing.status == 503, "stt_unavailable HTTP 503");
        check(Json::parse(missing.body).at("errors")[0].at("code") == "stt_unavailable", "stt_unavailable code");

        auto liveRec = std::make_unique<ScriptedRecognizer>();
        firefly::admin::AdminTranscriber live(std::move(liveRec));
        firefly::admin::Api api(store, client, &live);
        api.initializeRuntime();
        const auto beforeHash = store.current()->contentHash();
        auto reply = api.handle("POST", "/api/admin/transcribe", wavMonoS16(16000, 3200, 500));
        check(reply.status == 200, "transcribe HTTP 200");
        const auto body = Json::parse(reply.body);
        check(body.at("ok") == true && body.at("copilot_started") == false, "API transcript without Copilot");
        check(store.current()->contentHash() == beforeHash, "store unchanged after transcript");
        const auto status = Json::parse(api.handle("GET", "/api/status", "").body);
        check(status.at("stt_ready") == true, "status reports stt_ready");

        // Concurrent transcription → busy
        auto slow = std::make_unique<SlowRecognizer>();
        auto* slowPtr = slow.get();
        firefly::admin::AdminTranscriber busyStt(std::move(slow));
        std::thread first([&] { (void)busyStt.transcribe(wavMonoS16(16000, 1600)); });
        while (!slowPtr->entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto busy = Json::parse(busyStt.transcribe(wavMonoS16(16000, 1600)));
        first.join();
        check(busy.at("ok") == false && busy.at("errors")[0].at("code") == "stt_busy", "concurrent reject");

        std::cout << "AdminTranscribe: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AdminTranscribe failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
