#include "VoiceMode.h"
#include "CliSupport.h"
#include "firefly/RkllmEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/voice/AlsaAudioSource.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceProcessor.h"
#include "firefly/voice/VoiceLoop.h"
#include "firefly/runtime/RuntimeConfig.h"
#include "firefly/llm/LlmContext.h"
#include "firefly/actions/CompactV1Adapter.h"
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
        std::string stt, vad, device = "plughw:CARD=H340,DEV=0";
        std::string configPath = runtime::RuntimeConfig::defaultPath();
        bool explicitConfig = false;
        int runSeconds = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--voice") continue;
            if (arg == "--verbose") { rk.verbose = true; continue; }
            if (arg == "--help") {
                std::cout << "Voice: --voice --model PATH --stt-model-dir DIR --vad-model PATH\n"
                             "  [--audio-device ALSA_NAME] [--pre-roll-ms 300] [--post-roll-ms 300]\n"
                             "  [--max-utterance-ms 15000] [--voice-run-seconds N] [--verbose] [--timeout-ms N] [--config PATH]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("Missing value: " + arg);
            const std::string value = argv[++i];
            if (arg == "--model") rk.modelPath = value;
            else if (arg == "--config") {
                if (explicitConfig || value.empty()) throw std::invalid_argument("Expected one --config PATH");
                configPath = value;
                explicitConfig = true;
            }
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
        if (rk.modelPath.empty() || stt.empty() || vad.empty())
            throw std::invalid_argument("Voice mode requires --model, --stt-model-dir, --vad-model");
        const runtime::RuntimeConfig runtimeConfig(configPath);
        runtimeConfig.writeDiagnostics(std::cerr);
        const DeterministicParser parser(runtimeConfig.domain());
        rk.systemPrompt = llm::generateContext(runtimeConfig.domain()).systemPrompt();
        std::cerr << "PARSER_ROOMS authoritative=config action_model=generic\n";
        cli::CliOutput output;
        std::cerr << "Loading VAD...\n";
        SherpaVad detector(vad);
        VoiceSegmenter segmenter(detector, segments);
        std::cerr << "Loading offline STT...\n";
        SherpaRecognizer recognizer(stt);
        rk.progress = [](std::string_view stage) { std::cerr << stage << '\n'; };
        RkllmEngine llm(std::move(rk));
        CommandEngine engine(llm, parser, runtimeConfig.domain());
        const firefly::actions::CompactV1Adapter outputCodec(runtimeConfig.domain().snapshot());
        VoiceProcessor processor(recognizer, engine);
        if (stopped) return 0;
        AlsaAudioSource capture(device);
        auto emit = [&](const VoiceResult& r) {
            const auto compact = outputCodec.render(r.action);
            std::cerr << "VOICE speech=" << r.speechSeconds << "s duration=" << r.durationSeconds << "s\n"
                      << "STT: " << r.recognition.text << "\nSTT_MS=" << r.recognition.latencyMs
                      << "\nROUTE=" << r.route << "\nACTION=" << compact << '\n';
            if (!r.error.empty()) std::cerr << "VOICE_ERROR: " << r.error << '\n';
            output.write(compact + "\n");
        };
        std::cerr << "LISTENING device=" << device << " pre_ms=" << segments.preMs
                  << " post_ms=" << segments.postMs << '\n';
        const auto start = std::chrono::steady_clock::now();
        runVoiceLoop(capture, segmenter, [&](const Utterance& u) { return processor.process(u); }, emit,
            [](const std::string& state) { std::cerr << "VOICE_STATE=" << state << '\n'; },
            [&] { return stopped || (runSeconds && std::chrono::steady_clock::now() - start >= std::chrono::seconds(runSeconds)); });
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Voice error: " << e.what() << '\n';
        return 1;
    }
}
