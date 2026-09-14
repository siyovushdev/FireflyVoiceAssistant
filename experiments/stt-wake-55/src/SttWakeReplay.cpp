#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceSegmenter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace firefly::voice;

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double cpuMs() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0
        + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}

long peakRssKiB() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
    return usage.ru_maxrss;
}

long rssKiB() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") { long value = 0; status >> value; return value; }
        std::string rest;
        std::getline(status, rest);
    }
    throw std::runtime_error("Cannot read current RSS");
}

std::string json(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 0x20) {
            const char* digits = "0123456789abcdef";
            result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15];
        } else result += c;
    }
    return result + '"';
}

// Stream original PCM with bounded memory, including multi-minute recordings.
// No resampling, audio normalization, endpoint flush or synthetic tail padding.
class WaveReader {
public:
    explicit WaveReader(const std::string& path) : input_(path, std::ios::binary) {
        if (!input_ || tag() != "RIFF") throw std::runtime_error("Not a RIFF WAV: " + path);
        const auto riffBytes = integer(4);
        if (tag() != "WAVE") throw std::runtime_error("Not a WAVE file");
        input_.seekg(0, std::ios::end);
        const auto fileBytes = input_.tellg();
        const uint64_t riffEnd = uint64_t(riffBytes) + 8;
        if (fileBytes < 12 || riffEnd > uint64_t(fileBytes)) throw std::runtime_error("Truncated RIFF");
        input_.seekg(12);
        bool formatSeen = false;
        while (input_ && uint64_t(input_.tellg()) + 8 <= riffEnd) {
            const auto kind = tag();
            const auto bytes = integer(4);
            const auto start = input_.tellg();
            if (uint64_t(start) + bytes > riffEnd) throw std::runtime_error("Truncated WAV chunk");
            if (kind == "fmt ") {
                if (bytes < 16) throw std::runtime_error("Invalid fmt chunk");
                const auto format = integer(2), channels = integer(2), rate = integer(4);
                const auto byteRate = integer(4), alignment = integer(2), bits = integer(2);
                if (format != 1 || channels != 1 || rate != SampleRate || bits != 16
                    || alignment != 2 || byteRate != SampleRate * 2)
                    throw std::runtime_error("WAV must be mono 16000 Hz PCM S16_LE");
                formatSeen = true;
            } else if (kind == "data") {
                if (!formatSeen || bytes == 0 || bytes % 2) throw std::runtime_error("Invalid WAV data");
                remaining_ = bytes / 2;
                total_ = remaining_;
                return;
            }
            input_.seekg(start + std::streamoff(uint64_t(bytes) + bytes % 2));
        }
        throw std::runtime_error("WAV missing PCM data");
    }

    int read(std::array<float, FrameSamples>& frame) {
        const auto count = std::min<uint32_t>(FrameSamples, remaining_);
        for (uint32_t i = 0; i < count; ++i) {
            const int value = integer(2);
            frame[i] = (value >= 32768 ? value - 65536 : value) / 32768.0f;
        }
        remaining_ -= count;
        return count;
    }
    double seconds() const { return double(total_) / SampleRate; }
private:
    uint32_t integer(int bytes) {
        uint32_t value = 0;
        for (int i = 0; i < bytes; ++i) {
            const int c = input_.get();
            if (c == EOF) throw std::runtime_error("Unexpected WAV EOF");
            value |= uint32_t(c) << (8 * i);
        }
        return value;
    }
    std::string tag() {
        std::string value;
        for (int i = 0; i < 4; ++i) value += char(integer(1));
        return value;
    }
    std::ifstream input_;
    uint32_t remaining_ = 0, total_ = 0;
};

// Instrument calls without changing vendor parameters or segment decisions.
class TimedVad final : public SpeechDetector {
public:
    explicit TimedVad(SherpaVad& vad) : vad_(vad) {}
    Detection accept(const float* samples, int count) override {
        const auto start = Clock::now();
        auto result = vad_.accept(samples, count);
        const auto elapsed = milliseconds(Clock::now() - start);
        totalMs += elapsed; maxMs = std::max(maxMs, elapsed); ++calls;
        received += count;
        return result;
    }
    void reset() override { vad_.reset(); received = 0; }
    double totalMs = 0, maxMs = 0;
    int64_t received = 0, calls = 0;
private:
    SherpaVad& vad_;
};

void replay(const std::string& file, SherpaVad& detector, SherpaRecognizer& recognizer, bool paced) {
    WaveReader wave(file);
    detector.reset();
    TimedVad timed(detector);
    VoiceSegmenter segmenter(timed); // Existing 300/300 ms, 15 s maximum.
    const auto start = Clock::now();
    const auto cpuStart = cpuMs();
    const auto rssStart = rssKiB();
    int64_t sourceSamples = 0;
    int events = 0, sttCalls = 0, errors = 0;
    double sttMs = 0, sttCpuMs = 0, maxDeliveryLagMs = 0;
    std::array<float, FrameSamples> frame{};
    int count;
    while ((count = wave.read(frame)) != 0) {
        sourceSamples += count;
        if (paced) {
            const auto due = start + std::chrono::microseconds(sourceSamples * 1000000 / SampleRate);
            std::this_thread::sleep_until(due);
            maxDeliveryLagMs = std::max(maxDeliveryLagMs, milliseconds(Clock::now() - due));
        }
        for (auto& utterance : segmenter.accept(frame.data(), count)) {
            ++events;
            RecognitionResult result;
            const auto recognitionCpuStart = cpuMs();
            if (utterance.error.empty()) {
                result = recognizer.recognize(utterance.audio);
                ++sttCalls;
                sttMs += result.latencyMs;
            } else result.error = utterance.error;
            const auto recognitionCpu = cpuMs() - recognitionCpuStart;
            sttCpuMs += recognitionCpu;
            if (!result.success) ++errors;
            std::cout << "{\"type\":\"utterance\",\"file\":" << json(file)
                << ",\"event\":" << events << ",\"source_available_s\":" << double(sourceSamples) / SampleRate
                << ",\"speech_s\":" << double(utterance.speech.end - utterance.speech.start) / SampleRate
                << ",\"padded_s\":" << double(utterance.audio.size()) / SampleRate
                << ",\"vad_endpoint_to_available_ms\":";
            if (!utterance.error.empty()) std::cout << "null";
            else std::cout << double(timed.received - utterance.speech.end) * 1000 / SampleRate;
            std::cout << ",\"raw_transcript\":" << json(result.text)
                << ",\"success\":" << (result.success ? "true" : "false")
                << ",\"error\":" << json(result.error) << ",\"stt_ms\":" << result.latencyMs
                << ",\"stt_cpu_ms\":" << recognitionCpu << "}" << std::endl;
        }
    }
    const auto cpu = cpuMs() - cpuStart;
    const auto wall = milliseconds(Clock::now() - start);
    std::cout << "{\"type\":\"sample\",\"file\":" << json(file)
        << ",\"audio_s\":" << wave.seconds() << ",\"paced\":" << (paced ? "true" : "false")
        << ",\"events\":" << events << ",\"stt_calls\":" << sttCalls << ",\"errors\":" << errors
        << ",\"active_at_eof\":" << (segmenter.active() ? "true" : "false")
        << ",\"vad_calls\":" << timed.calls << ",\"vad_compute_ms\":" << timed.totalMs
        << ",\"vad_frame_max_ms\":" << timed.maxMs << ",\"stt_ms\":" << sttMs
        << ",\"stt_cpu_ms\":" << sttCpuMs << ",\"wall_ms\":" << wall << ",\"cpu_ms\":" << cpu
        << ",\"cpu_percent_one_core_wall\":" << cpu * 100 / wall
        << ",\"cpu_percent_one_core_audio_estimate\":" << cpu / (wave.seconds() * 10)
        << ",\"max_delivery_lag_ms\":" << maxDeliveryLagMs
        << ",\"rss_start_kib\":" << rssStart << ",\"rss_end_kib\":" << rssKiB()
        << ",\"process_peak_rss_kib_cumulative\":" << peakRssKiB() << "}" << std::endl;
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 5 || (std::string(argv[3]) != "--paced" && std::string(argv[3]) != "--unpaced"))
            throw std::invalid_argument("Usage: stt-wake-replay STT_DIR VAD_MODEL --paced|--unpaced WAV...");
        std::cout << std::fixed << std::setprecision(3);
        const auto start = Clock::now();
        const auto cpuStart = cpuMs();
        SherpaVad detector(argv[2]);
        SherpaRecognizer recognizer(argv[1]);
        std::cout << "{\"type\":\"initialization\",\"wall_ms\":" << milliseconds(Clock::now() - start)
            << ",\"cpu_ms\":" << cpuMs() - cpuStart << ",\"rss_kib\":" << rssKiB()
            << ",\"process_peak_rss_kib_cumulative\":" << peakRssKiB() << "}" << std::endl;
        for (int i = 4; i < argc; ++i) replay(argv[i], detector, recognizer, std::string(argv[3]) == "--paced");
    } catch (const std::exception& error) {
        std::cerr << "Evaluation failed: " << error.what() << '\n';
        return 1;
    }
}
