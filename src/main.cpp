#ifdef FIREFLY_WITH_VOICE
#include "VoiceMode.h"
#endif
#include "firefly/CommandEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/RkllmEngine.h"
#include "firefly/InputPolicy.h"
#include "firefly/runtime/RuntimeConfig.h"
#include "CliSupport.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
using firefly::cli::CliOutput;
using firefly::cli::readFile;
void usage() {
    std::cout << "Usage: firefly-assistant --model PATH --system-prompt PATH [--config PATH] [--verbose] [--timeout-ms N]\n";
}
}
int main(int argc, char** argv) {
#ifdef FIREFLY_WITH_VOICE
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--voice") return runVoiceMode(argc, argv);
#endif
    try {
        firefly::RkllmConfig config;
        std::string promptPath;
        std::string configPath = firefly::runtime::RuntimeConfig::defaultPath();
        bool explicitConfig = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--help") { usage(); return 0; }
            if (arg == "--config") {
                if (explicitConfig || i + 1 == argc || std::string(argv[i + 1]).empty())
                    throw std::invalid_argument("Expected one --config PATH");
                configPath = argv[++i];
                explicitConfig = true;
                continue;
            }
            if (arg == "--timeout-ms" && i + 1 < argc) {
                const std::string value(argv[++i]);
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("Timeout must be positive milliseconds");
                config.requestTimeout = std::chrono::milliseconds(std::stoll(value));
                continue;
            }
            if (arg == "--verbose") { config.verbose = true; continue; }
            if ((arg == "--model" || arg == "--system-prompt") && i + 1 < argc) {
                if (arg == "--model") config.modelPath = argv[++i];
                else promptPath = argv[++i];
            } else throw std::invalid_argument("Unknown option or missing value: " + arg);
        }
        if (config.modelPath.empty() || promptPath.empty()) {
            usage();
            return 2;
        }
        const firefly::runtime::RuntimeConfig runtimeConfig(configPath);
        runtimeConfig.writeDiagnostics(std::cerr);
        const firefly::DeterministicParser parser(runtimeConfig.domain());
        parser.requireLegacyRoomDomain();
        std::cerr << "PARSER_ROOMS authoritative=config legacy_shadow=matched\n";
        config.systemPrompt = readFile(promptPath);
        const bool verbose = config.verbose;
        config.progress = [](std::string_view stage) { std::cerr << stage << std::endl; };
        CliOutput output;
        firefly::RkllmEngine llm(std::move(config));
        firefly::CommandEngine engine(llm, parser, runtimeConfig.domain());
        std::cerr << "READY" << std::endl;
        const bool interactive = isatty(STDIN_FILENO);
        std::string line;
        while (true) {
            if (interactive) std::cerr << "> " << std::flush;
            if (!firefly::readInputLine(std::cin, line)) break;
            const auto first = line.find_first_not_of(" \t\r\n\f\v");
            if (first != std::string::npos && line.size() <= firefly::MaxInputBytes) {
                const auto last = line.find_last_not_of(" \t\r\n\f\v");
                const auto control = line.substr(first, last - first + 1);
                if (control == "quit" || control == "exit") break;
            }
            const auto result = engine.process(line);
            output.write(result.compact() + "\n");
            if (!result.error.empty()) std::cerr << "Rejected: " << result.error << '\n';
            if (result.latencyMs >= 2000) std::cerr << "Slow request: " << result.latencyMs << " ms\n";
            if (verbose) {
                std::cerr << (result.path == firefly::CommandPath::FastPath ? "FAST PATH" :
                              result.path == firefly::CommandPath::LlmFallback ? "LLM FALLBACK" : "INPUT REJECTED")
                          << " final=" << result.compact() << " command_ms=" << result.latencyMs << '\n';
            }
        }
        if (std::cin.bad()) throw std::runtime_error("Failed reading stdin");
        return 0;
    } catch (const std::exception& error) {
        std::cout << "U" << std::endl;
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
