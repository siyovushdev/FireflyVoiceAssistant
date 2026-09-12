#include "VoiceMode.h"
#include "CliSupport.h"
#include "firefly/RkllmEngine.h"
#include "firefly/voice/AlsaAudioSource.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceProcessor.h"
#include <array>
#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <stdexcept>
namespace {
// The signal handler only sets this flag; all model/audio cleanup runs in normal code.
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
struct Signals {
    using Handler = void (*)(int);
    Handler oldInt, oldTerm;
    Signals() { stopped = 0; oldInt = std::signal(SIGINT, stop); oldTerm = std::signal(SIGTERM, stop); }
    ~Signals() { std::signal(SIGINT, oldInt); std::signal(SIGTERM, oldTerm); }
};
int number(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Expected nonnegative integer: " + s);
    return std::stoi(s);
}
}
int runVoiceMode(int argc, char** argv) {
    using namespace firefly;
    using namespace firefly::voice;
    try {
        Signals signals;
        RkllmConfig rk;
        SegmentConfig segments;
        std::string prompt, stt, vad, device = "plughw:CARD=H340,DEV=0";
        int runSeconds = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--voice") continue;
            if (arg == "--verbose") { rk.verbose = true; continue; }
            if (arg == "--help") {
                std::cout << "Voice: --voice --model PATH --system-prompt PATH --stt-model-dir DIR --vad-model PATH\n"
                             "  [--audio-device ALSA_NAME] [--pre-roll-ms 300] [--post-roll-ms 300]\n"
                             "  [--max-utterance-ms 15000] [--voice-run-seconds N] [--verbose] [--timeout-ms N]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("Missing value: " + arg);
            const std::string value = argv[++i];
            if (arg == "--model") rk.modelPath = value;
            else if (arg == "--system-prompt") prompt = value;
            else if (arg == "--stt-model-dir") stt = value;
            else if (arg == "--vad-model") vad = value;
            else if (arg == "--audio-device") device = value;
            else if (arg == "--pre-roll-ms") segments.preMs = number(value);
            else if (arg == "--post-roll-ms") segments.postMs = number(value);
            else if (arg == "--max-utterance-ms") segments.maxSpeechMs = number(value);
            else if (arg == "--voice-run-seconds") runSeconds = number(value);
            else if (arg == "--timeout-ms") rk.requestTimeout = std::chrono::milliseconds(number(value));
            else throw std::invalid_argument("Unknown voice option: " + arg);
        }
        if (rk.modelPath.empty() || prompt.empty() || stt.empty() || vad.empty())
            throw std::invalid_argument("Voice mode requires --model, --system-prompt, --stt-model-dir, --vad-model");
        cli::CliOutput output;
        std::cerr << "Loading VAD...\n";
        SherpaVad detector(vad);
        VoiceSegmenter segmenter(detector, segments);
        std::cerr << "Loading offline STT...\n";
        SherpaRecognizer recognizer(stt);
        rk.systemPrompt = cli::readFile(prompt);
        rk.progress = [](std::string_view stage) { std::cerr << stage << '\n'; };
        RkllmEngine llm(std::move(rk));
        CommandEngine engine(llm);
        VoiceProcessor processor(recognizer, engine);
        if (stopped) return 0;
        AlsaAudioSource capture(device);
        std::future<VoiceResult> worker; // destroyed/joined before all referenced engines
        auto emit = [&](const VoiceResult& r) {
            std::cerr << "VOICE speech=" << r.speechSeconds << "s duration=" << r.durationSeconds << "s\n"
                      << "STT: " << r.recognition.text << "\nSTT_MS=" << r.recognition.latencyMs
                      << "\nROUTE=" << r.route << "\nACTION=" << r.action << '\n';
            if (!r.error.empty()) std::cerr << "VOICE_ERROR: " << r.error << '\n';
            output.write(r.action + "\n");
        };
        std::cerr << "LISTENING device=" << device << " pre_ms=" << segments.preMs
                  << " post_ms=" << segments.postMs << '\n';
        const auto start = std::chrono::steady_clock::now();
        std::array<float, FrameSamples> frame{};
        int filled = 0;
        bool waitingQuiet = false;
        while (!stopped) {
            if (runSeconds && std::chrono::steady_clock::now() - start >= std::chrono::seconds(runSeconds)) break;
            if (worker.valid() && worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto result = worker.get();
                if (stopped) break;
                emit(result);
                // Audio spoken during PROCESS is never queued for later execution. Require
                // a fresh quiet interval before rearming, including a tail still in progress.
                segmenter.discontinuity(); filled = 0; waitingQuiet = true;
                std::cerr << "WAITING_FOR_QUIET\n";
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
                waitingQuiet = false; std::cerr << "LISTENING\n";
            }
            for (auto& u : utterances) {
                if (worker.valid()) {
                    VoiceResult rejected; rejected.error = "Speech while PROCESS busy was discarded"; emit(rejected);
                } else if (!u.error.empty()) {
                    waitingQuiet = true;
                    VoiceResult rejected; rejected.error = u.error; emit(rejected);
                } else {
                    std::cerr << "PROCESS\n";
                    worker = std::async(std::launch::async, [&processor, u = std::move(u)] { return processor.process(u); });
                }
            }
        }
        std::cerr << "Stopping voice capture; waiting for any in-flight inference...\n";
        // No detached vendor call and no action emitted after a stop request.
        if (worker.valid()) worker.get();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Voice error: " << e.what() << '\n';
        return 1;
    }
}
