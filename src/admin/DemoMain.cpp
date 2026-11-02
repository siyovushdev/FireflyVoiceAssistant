#include "HttpServer.h"
#include "firefly/admin/NetworkPolicy.h"
#include "firefly/admin/RuntimeWorker.h"
#include "RuntimeModel.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/config/ConfigLoader.h"
#ifdef FIREFLY_WITH_VOICE
#include "firefly/voice/AlsaAudioSource.h"
#include "firefly/voice/F2SpeechOutput.h"
#ifdef FIREFLY_TEST_VOICE_REPLAY
#include "ReplayInput.h"
#endif
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceLoop.h"
#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/voice/AminaWakeDetector.h"
#include "firefly/llm/StructuredOutput.h"
#endif
#include "firefly/voice/WakeDetector.h"
#include "firefly/voice/WakeAcknowledgement.h"
#include "firefly/voice/VoiceRecovery.h"
#include "firefly/voice/WakeDirectionClient.h"
#ifdef FIREFLY_TEST_SETUP_RESOURCES
#include "FakeSetupResources.h"
#endif
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
        std::string device = "plughw:CARD=Array,DEV=0", ttsDirectory, wakeFocusSocket;
        firefly::voice::WakeConfig wakeConfig;
        wakeConfig.afterBeepOnly = true;
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
                    "  [--stt-model-dir DIR] [--tts-model-dir DIR] (accepted F2 assets)\n"
                    "  [--voice --vad-model FILE --audio-device plughw:CARD=Array,DEV=0]\n"
                    "  [--wake-enabled true|false] [--wake-model FILE] [--wake-python EXECUTABLE]\n"
                    "  [--wake-cutoff 0.99] [--wake-window 7] [--command-start-timeout-ms 5000] [--playback-device DEVICE]\n"
                    "  [--wake-beep-enabled true|false] [--wake-beep-volume-percent 1..30] (defaults: true, 30)\n"
                    "  [--wake-focus-socket PRIVATE_SOCKET] (optional external direction experiment)\n"
                    "  [--wake-command-mode after-beep|auto] (default: after-beep; command follows beep)\n"
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
            else if (flag == "--tts-model-dir") ttsDirectory = value;
            else if (flag == "--vad-model") vad = value;
            else if (flag == "--audio-device") device = value;
            else if (flag == "--wake-enabled") {
                if (value != "true" && value != "false") throw std::invalid_argument("Invalid wake-enabled boolean");
                wakeConfig.enabled = value == "true";
            }
            else if (flag == "--wake-model") wakeConfig.modelPath = value;
            else if (flag == "--wake-focus-socket") wakeFocusSocket = value;
            else if (flag == "--wake-command-mode") {
                if (value != "after-beep" && value != "auto")
                    throw std::invalid_argument("Invalid wake-command-mode");
                wakeConfig.afterBeepOnly = value == "after-beep";
            }
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
        if (voice && wakeConfig.enabled && wakeConfig.afterBeepOnly && !acknowledgementConfig.enabled)
            throw std::invalid_argument("After-beep mode requires enabled wake beep");
        firefly::voice::WakeDirectionClient wakeDirection(wakeFocusSocket);
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
#ifdef FIREFLY_TEST_SETUP_RESOURCES
        auto resources = fakeSetupResources();
#else
        auto resources = std::make_shared<firefly::SetupResources>();
#endif
        firefly::admin::RuntimeModel model(modelPath, adminModel, resources);
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
                    std::make_unique<firefly::voice::SherpaRecognizer>(stt, firefly::voice::RecognitionProfile::AdminSetup), resources);
                api.attachTranscriber(transcriber.get());
                api.noteStt(true);
                std::cerr << "STARTUP Admin Copilot STT ready\n";
            } catch (const std::exception& error) {
                api.noteStt(false, "stt_init_failed");
                std::cerr << "STARTUP Admin Copilot STT unavailable: " << error.what() << '\n';
            }
        }
#endif

#ifdef FIREFLY_TEST_VOICE_REPLAY
        std::shared_ptr<firefly::voice::test::ReplayData> replay;
        if (const auto* file = std::getenv("FIREFLY_TEST_REPLAY_WAV"))
            replay = std::make_shared<firefly::voice::test::ReplayData>(file);
#endif
        firefly::voice::VoiceRecovery recovery;
#ifdef FIREFLY_WITH_VOICE
        // Access only inside the recovery audio lease, or its idle cleanup lock.
        std::unique_ptr<firefly::voice::SherpaVad> detector;
        std::unique_ptr<firefly::voice::SherpaRecognizer> recognizer;
        std::unique_ptr<firefly::voice::F2SpeechOutput> speaker;
#endif
        runtime.setAudioDrain([&] {
            recovery.drain([&] {
                auto lease = resources->acquire();
                resources->note("before_voice_release");
#ifdef FIREFLY_WITH_VOICE
                speaker.reset(); runtime.speechStatus("released");
                recognizer.reset();
                detector.reset();
#endif
                resources->note("after_voice_release");
            });
        });
        std::thread services([&] {
            // RKLLM admission happens here (Apply), with Admin already listening.
            api.initializeRuntime();
#ifdef FIREFLY_WITH_VOICE
            // CPU models survive device retries, but are released for Setup.
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
                    {
                        auto lease = resources->acquire();
                        if (interrupted()) return;
                        const bool loading = !detector || !recognizer;
                        if (loading) resources->checkpoint("before_voice_load", firefly::SetupResources::GiBKiB);
                        if (!detector) detector = std::make_unique<firefly::voice::SherpaVad>(vad);
                        if (interrupted()) return;
                        if (!recognizer) recognizer = std::make_unique<firefly::voice::SherpaRecognizer>(stt);
                        if (loading) {
                            try { resources->checkpoint("after_voice_load", firefly::SetupResources::GiBKiB); }
                            catch (...) {
                                recognizer.reset(); detector.reset();
                                resources->note("voice_load_rejected");
                                throw;
                            }
                        }
                        api.noteStt(true);
                    }
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
                    std::unique_ptr<firefly::voice::AudioSource> capture;
#ifdef FIREFLY_TEST_VOICE_REPLAY
                    if (replay) capture = std::make_unique<firefly::voice::test::ReplayInput>(replay);
                    else
#endif
                    capture = std::make_unique<firefly::voice::AlsaAudioSource>(device, 2, 1);
                    firefly::voice::AlsaWakeAcknowledgement acknowledgement(playbackDevice, acknowledgementConfig);
                    std::cerr << "VOICE_ACK enabled=" << (acknowledgementConfig.enabled ? "true" : "false")
                        << " volume_percent=" << acknowledgementConfig.volumePercent << '\n';
                    std::cerr << "VOICE_INPUT mode=" << (wakeConfig.afterBeepOnly ? "after-beep" : "auto") << '\n';
                    const auto process = [&](const firefly::voice::Utterance& u) {
                        const auto epoch = runtime.speechEpoch();
                        auto result = runtime.processVoice(*recognizer, u);
                        std::cerr << "VOICE_REPLY_READY monotonic_ms="
                            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
                        const auto cancelled = [&] {
                            return interrupted() || epoch != runtime.speechEpoch() || result.replyGeneration != runtime.voiceGenerationId() || runtime.voicePreferences().responses == "off";
                        };
                        if (result.replyText.empty() && result.replyReason == "state_confirmed" && !cancelled())
                            runtime.speechStatus("silent");
                        if (!result.replyText.empty() && result.replyMode != "off" && !cancelled()) {
                            try {
                                {
                                    auto lease = resources->acquire();
                                    if (!speaker) {
                                        speaker = std::make_unique<firefly::voice::F2SpeechOutput>(ttsDirectory, playbackDevice,
                                            [](const std::string& row) { std::cerr << ("F2_TRACE " + row + "\n"); }, "",
                                            [&](const std::function<void()>& load) {
                                                // Stock PCM never loads the model. Dynamic speech
                                                // uses the same resource admission and audio lease.
                                                auto loadingLease = resources->acquire();
                                                if (interrupted()) throw std::runtime_error("speech_cancelled");
                                                if (ttsDirectory.empty()) throw std::runtime_error("tts_model_not_configured");
                                                resources->checkpoint("before_f2_load", 2 * firefly::SetupResources::GiBKiB);
                                                load();
                                                resources->checkpoint("after_f2_load", firefly::SetupResources::GiBKiB);
                                            });
                                    }
                                }
                                runtime.speechStatus("speaking");
                                const auto spoken = speaker->speak(result.replyText, cancelled);
                                runtime.speechStatus(spoken.success ? "ready" : spoken.cancelled ? "cancelled" : "failed", spoken.error);
                            } catch (const std::exception&) {
                                speaker.reset(); runtime.speechStatus("failed", "tts_unavailable");
                                std::cerr << "VOICE_REPLY speech_failed text_result_retained=true\n";
                            }
                            // Processing retains capture ownership while PCM is played;
                            // original loop drains it, including a bounded acoustic tail.
                            for (int i=0;i<10&&!interrupted();++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        return result;
                    };
                    const auto emit = [](const firefly::voice::VoiceResult& result) {
                            std::cerr << "VOICE text=" << result.recognition.text << " route=" << result.route
                                << " result=" << firefly::llm::structuredResult(result.action)
                                << " inline=" << (result.inlineWake ? "true" : "false")
                                << " wake_only=" << (result.wakeOnly ? "true" : "false")
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
                        // Deliver directly before journal I/O and continuation probing.
                        // Failure is an optional acoustic enhancement miss, never a
                        // reason to authorize or repair a command.
                        if (wakeDirection.enabled()) (void)wakeDirection.notify(value);
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
                        firefly::voice::runWakeVoiceLoop(*capture, *wake, segmenter, wakeConfig,
                            process, emit, state, interrupted, std::chrono::steady_clock::now,
                            acknowledgementConfig.enabled ? &acknowledgement : nullptr);
                    else {
                        std::cerr << "VOICE wake gating explicitly disabled by configuration\n";
                        firefly::voice::runVoiceLoop(*capture, segmenter, process, emit, state, interrupted);
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
