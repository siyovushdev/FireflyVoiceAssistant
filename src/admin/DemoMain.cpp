#include "HttpServer.h"
#include "RuntimeModel.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/config/ConfigLoader.h"
#ifdef FIREFLY_WITH_VOICE
#include "firefly/voice/AlsaAudioSource.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceLoop.h"
#include "firefly/llm/StructuredOutput.h"
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
    try {
        std::string storePath, initial, bind = "127.0.0.1", modelPath, stt, vad;
        std::string device = "plughw:CARD=H340,DEV=0";
        unsigned short port = 8080;
        bool voice = false;
        for (int i = 1; i < argc; ++i) {
            const std::string flag(argv[i]);
            if (flag == "--help") {
                std::cout << "firefly-demo --store DIR --model FILE [--init-config FILE]\n"
                    "  [--admin-bind 127.0.0.1] [--admin-port 8080]\n"
                    "  [--stt-model-dir DIR]\n"
                    "  [--voice --vad-model FILE --audio-device plughw:CARD=H340,DEV=0]\n"
                    "One process appliance runtime supervised by systemd in production.\n"
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
            else if (flag == "--stt-model-dir") stt = value;
            else if (flag == "--vad-model") vad = value;
            else if (flag == "--audio-device") device = value;
            else if (flag == "--admin-port") {
                unsigned number;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number > 65535)
                    throw std::invalid_argument("Invalid admin port");
                port = static_cast<unsigned short>(number);
            } else throw std::invalid_argument("Unknown option: " + flag);
        }
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
        firefly::admin::RuntimeModel model(modelPath);
        firefly::admin::RuntimeWorker runtime(model);
        firefly::admin::LocalRuntimeClient client(runtime);
        firefly::admin::Api api(store, client, nullptr);
        api.health().setPhase("process_started");
        api.noteStt(false);

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
                    std::make_unique<firefly::voice::SherpaRecognizer>(stt));
                api.attachTranscriber(transcriber.get());
                api.noteStt(true);
                std::cerr << "STARTUP Admin Copilot STT ready\n";
            } catch (const std::exception& error) {
                api.noteStt(false, "stt_init_failed");
                std::cerr << "STARTUP Admin Copilot STT unavailable: " << error.what() << '\n';
            }
        }
#endif

        std::thread services([&] {
            // RKLLM admission happens here (Apply), with Admin already listening.
            api.initializeRuntime();
#ifdef FIREFLY_WITH_VOICE
            if (voice && !stopping) {
                try {
                    runtime.voiceStatus("loading", "opening");
                    firefly::voice::SherpaVad detector(vad);
                    firefly::voice::VoiceSegmenter segmenter(detector);
                    firefly::voice::SherpaRecognizer recognizer(stt);
                    firefly::voice::AlsaAudioSource capture(device);
                    firefly::voice::runVoiceLoop(capture, segmenter,
                        [&](const firefly::voice::Utterance& u) { return runtime.processVoice(recognizer, u); },
                        [](const firefly::voice::VoiceResult& result) {
                            std::cerr << "VOICE text=" << result.recognition.text << " route=" << result.route
                                << " result=" << firefly::llm::structuredResult(result.action)
                                << " error=" << result.error << '\n';
                        }, [&](const std::string& state) { runtime.voiceStatus(state, "connected"); },
                        [] { return stopping != 0; });
                    runtime.voiceStatus("stopped", "closed");
                } catch (const std::exception& error) {
                    runtime.voiceStatus("unavailable", "unavailable");
                    api.noteStt(false, error.what());
                    std::cerr << "STARTUP voice unavailable; admin/text remain available: " << error.what() << '\n';
                }
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
            throw;
        }
        // SIGTERM set `stopping`; teardown runs here on the main thread.
        api.beginShutdown();
        runtime.stop();
        services.join();
        std::cerr << "SHUTDOWN complete\n";
    } catch (const std::exception& error) {
        std::cerr << "Demo startup: " << error.what() << '\n';
        return 1;
    }
}
