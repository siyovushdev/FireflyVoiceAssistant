#include "firefly/voice/SherpaSpeech.h"
#include "c-api.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
namespace firefly::voice {
namespace {
template<class T, class D> auto own(const T* p, D deleter) {
    if (!p) throw std::runtime_error("sherpa-onnx returned null");
    return std::unique_ptr<const T, D>(p, deleter);
}
void checkFile(const std::string& p) {
    if (!std::filesystem::is_regular_file(p)) throw std::runtime_error("Missing speech model file: " + p);
}
}
struct SherpaVad::Impl {
    const SherpaOnnxVoiceActivityDetector* handle = nullptr;
    ~Impl() { if (handle) SherpaOnnxDestroyVoiceActivityDetector(handle); }
};
SherpaVad::SherpaVad(const std::string& model) : impl_(std::make_unique<Impl>()) {
    checkFile(model);
    SherpaOnnxVadModelConfig c{};
    c.sample_rate = SampleRate; c.num_threads = 1; c.provider = "cpu";
    c.silero_vad.model = model.c_str(); c.silero_vad.threshold = .5f;
    c.silero_vad.min_silence_duration = .5f; c.silero_vad.min_speech_duration = .25f;
    c.silero_vad.window_size = FrameSamples; c.silero_vad.max_speech_duration = 20;
    impl_->handle = SherpaOnnxCreateVoiceActivityDetector(&c, 30);
    if (!impl_->handle) throw std::runtime_error("Cannot initialize Silero VAD");
}
SherpaVad::~SherpaVad() = default;
Detection SherpaVad::accept(const float* p, int n) {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(impl_->handle, p, n);
    Detection result;
    result.active = SherpaOnnxVoiceActivityDetectorDetected(impl_->handle);
    while (!SherpaOnnxVoiceActivityDetectorEmpty(impl_->handle)) {
        auto segment = own(SherpaOnnxVoiceActivityDetectorFront(impl_->handle), SherpaOnnxDestroySpeechSegment);
        result.completed.push_back({segment->start, int64_t(segment->start) + segment->n});
        SherpaOnnxVoiceActivityDetectorPop(impl_->handle);
    }
    return result;
}
void SherpaVad::reset() { SherpaOnnxVoiceActivityDetectorReset(impl_->handle); }
struct SherpaRecognizer::Impl {
    const SherpaOnnxOfflineRecognizer* handle = nullptr;
    ~Impl() { if (handle) SherpaOnnxDestroyOfflineRecognizer(handle); }
};
SherpaRecognizer::SherpaRecognizer(const std::string& directory) : impl_(std::make_unique<Impl>()) {
    const std::string encoder = directory + "/encoder.int8.onnx", decoder = directory + "/decoder.onnx",
                      joiner = directory + "/joiner.int8.onnx", tokens = directory + "/tokens.txt";
    for (const auto& p : {encoder, decoder, joiner, tokens}) checkFile(p);
    SherpaOnnxOfflineRecognizerConfig c{};
    c.feat_config.sample_rate = SampleRate; c.feat_config.feature_dim = 80;
    c.model_config.transducer.encoder = encoder.c_str(); c.model_config.transducer.decoder = decoder.c_str();
    c.model_config.transducer.joiner = joiner.c_str(); c.model_config.tokens = tokens.c_str();
    c.model_config.provider = "cpu"; c.model_config.num_threads = 2;
    c.decoding_method = "greedy_search"; c.max_active_paths = 4;
    impl_->handle = SherpaOnnxCreateOfflineRecognizer(&c);
    if (!impl_->handle) throw std::runtime_error("Cannot initialize Russian Zipformer");
}
SherpaRecognizer::~SherpaRecognizer() = default;
RecognitionResult SherpaRecognizer::recognize(const std::vector<float>& audio) {
    const auto start = std::chrono::steady_clock::now();
    RecognitionResult r;
    try {
        if (audio.empty() || audio.size() > 17 * SampleRate) throw std::runtime_error("Invalid STT audio length");
        for (float sample : audio)
            if (!std::isfinite(sample) || sample < -1 || sample > 1) throw std::runtime_error("Invalid PCM sample");
        auto stream = own(SherpaOnnxCreateOfflineStream(impl_->handle), SherpaOnnxDestroyOfflineStream);
        SherpaOnnxAcceptWaveformOffline(stream.get(), SampleRate, audio.data(), audio.size());
        // Match sherpa-onnx-offline: its DecodeStreams path with a batch of one.
        const auto* ptr = stream.get();
        SherpaOnnxDecodeMultipleOfflineStreams(impl_->handle, &ptr, 1);
        auto result = own(SherpaOnnxGetOfflineStreamResult(stream.get()), SherpaOnnxDestroyOfflineRecognizerResult);
        if (!result->text) throw std::runtime_error("STT returned no text");
        r.text = result->text; r.success = true;
    } catch (const std::exception& e) { r.text.clear(); r.error = e.what(); }
    r.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return r;
}
}
