#include "HttpServer.h"
#include "firefly/admin/NetworkPolicy.h"
#include "firefly/admin/RuntimeWorker.h"
#include "RuntimeModel.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/config/ConfigLoader.h"
#ifdef FIREFLY_WITH_VOICE
#include "firefly/voice/AlsaAudioSource.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceLoop.h"
#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/voice/AminaWakeDetector.h"
#include "firefly/llm/StructuredOutput.h"
#endif
#include "firefly/voice/WakeDetector.h"
#include "firefly/voice/WakeAcknowledgement.h"
#include "firefly/voice/VoiceRecovery.h"
#include <charconv>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
// Async-signal-safe only: write sig_atomic_t. No mutex, allocation, or C++ teardown.
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--build-identity") {
        std::cout << firefly::admin::buildIdentityJson();
        return 0;
    }
    try {
        std::string storePath, initial, bind = "127.0.0.1", modelPath, adminModel, stt, vad;
        std::string device = "plughw:CARD=Array,DEV=0";
        firefly::voice::WakeConfig wakeConfig;
        firefly::voice::WakeAcknowledgementConfig acknowledgementConfig;
        std::string wakePython = "python3", playbackDevice = "plughw:CARD=rockchipes8388,DEV=0";
        const auto executableDir = std::filesystem::canonical("/proc/self/exe").parent_path();
        auto wakeData = executableDir / "share/firefly/wake";
        if (!std::filesystem::exists(wakeData)) wakeData = executableDir.parent_path() / "share/firefly/wake";
        wakeConfig.modelPath = (wakeData / "amina-v3.tflite").string();
        unsigned short port = 8080;
        bool voice = false;
        for (int i = 1; i < argc; ++i) {
            const std::string flag(argv[i]);
            if (flag == "--help") {
                std::cout << "firefly-demo --store DIR --model FILE [--admin-model FILE] [--init-config FILE]\n"
                    "  [--admin-bind 127.0.0.1] [--admin-port 8080]\n"
                    "  [--stt-model-dir DIR]\n"
                    "  [--voice --vad-model FILE --audio-device plughw:CARD=Array,DEV=0]\n"
                    "  [--wake-enabled true|false] [--wake-model FILE] [--wake-python EXECUTABLE]\n"
                    "  [--wake-cutoff 0.99] [--wake-window 7] [--command-start-timeout-ms 5000] [--playback-device DEVICE]\n"
                    "  [--wake-beep-enabled true|false] [--wake-beep-volume-percent 1..30] (defaults: true, 30)\n"
                    "  [--command-listen-timeout-ms MS] (deprecated total listening cap)\n"
                    "One process appliance runtime supervised by systemd in production.\n"
                    "V1 Admin is loopback-only (127.0.0.1); remote access requires an SSH/Tailscale TCP tunnel.\n"
                    "DIR must be private/user-owned. HTTP status remains available when LLM/STT fail.\n";
                return 0;
            }
            if (flag == "--voice") { voice = true; continue; }
            if (++i == argc) throw std::invalid_argument("Missing option value");
            const std::string value(argv[i]);
            if (flag == "--store") storePath = value;
            else if (flag == "--init-config") initial = value;
            else if (flag == "--admin-bind") bind = value;
            else if (flag == "--model") modelPath = value;
            else if (flag == "--admin-model") {
                if (value.empty()) throw std::invalid_argument("Invalid admin model path");
                adminModel = value;
            }
            else if (flag == "--stt-model-dir") stt = value;
            else if (flag == "--vad-model") vad = value;
            else if (flag == "--audio-device") device = value;
            else if (flag == "--wake-enabled") {
                if (value != "true" && value != "false") throw std::invalid_argument("Invalid wake-enabled boolean");
                wakeConfig.enabled = value == "true";
            }
            else if (flag == "--wake-model") wakeConfig.modelPath = value;
            else if (flag == "--playback-device") playbackDevice = value;
            else if (flag == "--wake-beep-enabled") {
                if (value != "true" && value != "false") throw std::invalid_argument("Invalid wake-beep-enabled boolean");
                acknowledgementConfig.enabled = value == "true";
            }
            else if (flag == "--wake-beep-volume-percent") {
                int number;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    throw std::invalid_argument("Invalid wake beep volume percent");
                acknowledgementConfig.volumePercent = number;
                acknowledgementConfig.validate();
            }
            else if (flag == "--wake-python") wakePython = value;
            else if (flag == "--wake-cutoff") {
                if (value != "0.99") throw std::invalid_argument("V1 wake cutoff is frozen at 0.99");
            }
            else if (flag == "--wake-window") {
                if (value != "7") throw std::invalid_argument("V1 wake window is frozen at 7");
            }
            else if (flag == "--command-listen-timeout-ms" || flag == "--command-start-timeout-ms") {
                int number;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    throw std::invalid_argument("Invalid command listen timeout");
                if (flag == "--command-start-timeout-ms") wakeConfig.commandStartTimeout = std::chrono::milliseconds(number);
                else {
                    if (number == 0) throw std::invalid_argument("Legacy timeout must be positive");
                    wakeConfig.commandTimeout = std::chrono::milliseconds(number);
                    std::cerr << "Deprecated --command-listen-timeout-ms retains a TOTAL session deadline; prefer --command-start-timeout-ms\n";
                }
            }
            else if (flag == "--admin-port") {
                unsigned number;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number > 65535)
                    throw std::invalid_argument("Invalid admin port");
                port = static_cast<unsigned short>(number);
            } else throw std::invalid_argument("Unknown option: " + flag);
        }
        acknowledgementConfig.validate();
        firefly::admin::validateAdminBind(bind);
        // A bad Admin model must not load, and must not take down the listener.
        // enterSetup repeats the size check and requires the SHA256 before inference.
        if (!adminModel.empty()) {
            try { firefly::admin::RuntimeModel::checkAdminModelFile(adminModel); }
            catch (const std::exception& error) {
                std::cerr << "STARTUP admin model unavailable: " << error.what() << '\n';
            }
        }
#ifdef FIREFLY_WITH_VOICE
        if (stt.empty()) {
            const auto bundled = executableDir / "stt/model";
            if (std::filesystem::is_directory(bundled)) stt = bundled.string();
        }
#endif
        wakeConfig.validate();
        if (storePath.empty()) throw std::invalid_argument("--store is required");
#ifndef FIREFLY_WITH_VOICE
        if (voice || !stt.empty()) throw std::invalid_argument("Build with FIREFLY_BUILD_VOICE=ON for STT/voice");
#else
        if (voice && (stt.empty() || vad.empty() || modelPath.empty()))
            throw std::invalid_argument("Voice requires model, STT and VAD paths");
#endif
        std::cerr << "STARTUP process started\n";
        firefly::config::ConfigStore store(storePath);
        if (!initial.empty() && !std::filesystem::exists(std::filesystem::symlink_status(
                std::filesystem::path(storePath) / "configuration.json")))
            store.initialize(firefly::config::ConfigLoader::fromFile(initial));
        // RuntimeModel defers RKLLM load until Apply/useContext — after Admin listens below.
        firefly::admin::RuntimeModel model(modelPath, adminModel);
        firefly::admin::RuntimeWorker runtime(model);
        firefly::admin::LocalRuntimeClient client(runtime);
        firefly::admin::Api api(store, client, nullptr);
        api.health().setPhase("process_started");
        api.noteStt(false);
        using firefly::admin::SubsystemStatus;
        api.health().setSubsystem("wake", voice && wakeConfig.enabled ? SubsystemStatus::Starting : SubsystemStatus::Unavailable,
            {}, voice && !wakeConfig.enabled ? "explicitly_disabled" : "");

        firefly::admin::HttpServer server(api, bind, port);
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        api.noteAdminListening();
        std::cout << "DEMO_LISTEN http://" << bind << ':' << server.port() << '\n' << std::flush;

        // STT after Admin bind so init exceptions cannot block status/recovery surface.
        std::unique_ptr<firefly::admin::AdminTranscriber> transcriber;
#ifdef FIREFLY_WITH_VOICE
        if (!stt.empty()) {
            try {
                transcriber = std::make_unique<firefly::admin::AdminTranscriber>(
                    std::make_unique<firefly::voice::SherpaRecognizer>(stt, firefly::voice::RecognitionProfile::AdminSetup));
                api.attachTranscriber(transcriber.get());
                api.noteStt(true);
                std::cerr << "STARTUP Admin Copilot STT ready\n";
            } catch (const std::exception& error) {
                api.noteStt(false, "stt_init_failed");
                std::cerr << "STARTUP Admin Copilot STT unavailable: " << error.what() << '\n';
            }
        }
#endif

        firefly::voice::VoiceRecovery recovery;
        runtime.setAudioDrain([&] { recovery.drain(); });
        std::thread services([&] {
            // RKLLM admission happens here (Apply), with Admin already listening.
            api.initializeRuntime();
#ifdef FIREFLY_WITH_VOICE
            // CPU models survive device retries. VAD state is reset for every
            // attempt; recognition streams are already local to each request.
            std::unique_ptr<firefly::voice::SherpaVad> detector;
            std::unique_ptr<firefly::voice::SherpaRecognizer> recognizer;
            const auto interrupted = [&] { return stopping != 0 || runtime.setupBlocked(); };
            if (voice) recovery.run({
                [&] { return stopping != 0; },
                [&] { return runtime.setupBlocked(); },
                [&] {
                    runtime.voiceStatus("loading", "opening");
                    if (wakeConfig.enabled)
                        api.health().setSubsystem("wake", SubsystemStatus::Starting, {}, "opening");
                },
                [&](const std::function<void()>& ready) {
                    if (interrupted()) return;
                    if (!detector) detector = std::make_unique<firefly::voice::SherpaVad>(vad);
                    if (interrupted()) return;
                    if (!recognizer) recognizer = std::make_unique<firefly::voice::SherpaRecognizer>(stt);
                    if (interrupted()) return;
                    firefly::voice::VoiceSegmenter segmenter(*detector);
                    segmenter.startSession();
                    std::unique_ptr<firefly::voice::AminaWakeDetector> wake;
                    if (wakeConfig.enabled)
                        wake = std::make_unique<firefly::voice::AminaWakeDetector>(wakeConfig, wakePython,
                            (wakeData / "amina_worker.py").string());
                    if (interrupted()) return;
                    // Preserve wake-before-capture order: ALSA starts immediately,
                    // so loading a worker afterwards could overrun its short buffer.
                    // One capture owner for wake AND command audio, ASR channel 1.
                    firefly::voice::AlsaAudioSource capture(device, 2, 1);
                    firefly::voice::AlsaWakeAcknowledgement acknowledgement(playbackDevice, acknowledgementConfig);
                    std::cerr << "VOICE_ACK enabled=" << (acknowledgementConfig.enabled ? "true" : "false")
                        << " volume_percent=" << acknowledgementConfig.volumePercent << '\n';
                    const auto process = [&](const firefly::voice::Utterance& u) { return runtime.processVoice(*recognizer, u); };
                    const auto emit = [](const firefly::voice::VoiceResult& result) {
                            std::cerr << "VOICE text=" << result.recognition.text << " route=" << result.route
                                << " result=" << firefly::llm::structuredResult(result.action)
                                << " inline=" << (result.inlineWake ? "true" : "false")
                                << " wake_prefix_removed=" << (result.wakePrefixRemoved ? "true" : "false")
                                << " command_text=" << result.commandText
                                << " error=" << result.error
                                << " audio_s=" << result.durationSeconds << " speech_s=" << result.speechSeconds
                                << " stt_ms=" << result.recognition.latencyMs
                                << " llm_context_ms=" << result.llmTimings.contextMs
                                << " llm_prefill_ms=" << result.llmTimings.prefillMs
                                << " llm_generation_ms=" << result.llmTimings.generationMs
                                << " llm_output_tokens=" << result.llmTimings.outputTokens
                                << " processing_ms=" << result.processingMs << '\n';
                        };
                    const auto state = [&](const std::string& value) {
                        runtime.voiceStatus(value, "connected");
                        std::cerr << "VOICE_STATE " << value << " monotonic_ms="
                            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (value == "CONTINUATION_INLINE" || value == "CONTINUATION_SEPARATED")
                            std::cerr << " inline=" << (value == "CONTINUATION_INLINE" ? "true" : "false");
                        std::cerr << '\n';
                        if (value == "ACKNOWLEDGEMENT_FAILED") {
                            api.health().setSubsystem("acknowledgement", SubsystemStatus::Failed, "playback_failed", acknowledgement.error());
                            std::cerr << "VOICE_ACK failure=" << acknowledgement.error() << '\n';
                        } else if (value == "BEEP_END") api.health().setSubsystem("acknowledgement", SubsystemStatus::Ready, {}, value);
                        if (wakeConfig.enabled) {
                            const bool unavailable = value == "WAKE_UNAVAILABLE";
                            api.health().setSubsystem("wake", unavailable ? SubsystemStatus::Failed : SubsystemStatus::Ready,
                                unavailable ? "wake_path_failed" : "", value);
                        }
                        if (value == "WAITING_WAKE" || (!wakeConfig.enabled && value == "listening")) ready();
                    };
                    if (wakeConfig.enabled)
                        firefly::voice::runWakeVoiceLoop(capture, *wake, segmenter, wakeConfig,
                            process, emit, state, interrupted, std::chrono::steady_clock::now,
                            acknowledgementConfig.enabled ? &acknowledgement : nullptr);
                    else {
                        std::cerr << "VOICE wake gating explicitly disabled by configuration\n";
                        firefly::voice::runVoiceLoop(capture, segmenter, process, emit, state, interrupted);
                    }
                    runtime.voiceStatus("stopped", "closed");
                    api.health().setSubsystem("wake", SubsystemStatus::Unavailable, {}, "stopped");
                },
                [&](const std::string& error) {
                    runtime.voiceStatus("unavailable", "unavailable");
                    if (wakeConfig.enabled)
                        api.health().setSubsystem("wake", SubsystemStatus::Failed, "wake_voice_unavailable", error);
                    if (!detector || !recognizer) api.noteStt(false, error);
                },
                [](const std::string& event) { std::cerr << "VOICE_RECOVERY " << event << '\n'; }
            });
            if (voice) {
                runtime.voiceStatus("stopped", "closed");
                api.health().setSubsystem("wake", SubsystemStatus::Unavailable, {}, "stopped");
            }
#endif
        });
        try { server.run(stopping); }
        catch (...) {
            stopping = 1;
            // Normal thread context only — never from the signal handler.
            api.beginShutdown();
            runtime.stop();
            services.join();
            server.finishClients();
            throw;
        }
        // SIGTERM set `stopping`; teardown runs here on the main thread.
        api.beginShutdown();
        runtime.stop();
        services.join();
        server.finishClients();
        std::cerr << "SHUTDOWN complete\n";
    } catch (const std::exception& error) {
        std::cerr << "Demo startup: " << error.what() << '\n';
        return 1;
    }
}
