#include "firefly/CommandEngine.h"
#include "firefly/RkllmEngine.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
// CLI-only descriptor routing: vendor stdout goes to stderr while results retain
// the original stdout. This never changes logging inside the reusable engine.
class CliOutput {
public:
    CliOutput() {
        std::cout.flush();
        const int saved = dup(STDOUT_FILENO);
        if (saved < 0) throw std::runtime_error("Cannot preserve stdout");
        results_ = fdopen(saved, "w");
        if (!results_) { close(saved); throw std::runtime_error("Cannot open result stream"); }
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            fclose(results_);
            throw std::runtime_error("Cannot route runtime diagnostics");
        }
    }
    ~CliOutput() {
        fflush(stdout);
        dup2(fileno(results_), STDOUT_FILENO);
        fclose(results_);
    }
    CliOutput(const CliOutput&) = delete;
    CliOutput& operator=(const CliOutput&) = delete;
    void write(const std::string& text) {
        if (fwrite(text.data(), 1, text.size(), results_) != text.size() || fflush(results_) != 0)
            throw std::runtime_error("Cannot write CLI output");
    }
private:
    FILE* results_ = nullptr;
};
std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open system prompt: " + path);
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) throw std::runtime_error("Cannot read system prompt: " + path);
    return contents.str();
}
void usage() {
    std::cout << "Usage: firefly-assistant --model PATH --system-prompt PATH [--verbose]\n";
}
}
int main(int argc, char** argv) {
    try {
        firefly::RkllmConfig config;
        std::string promptPath;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--help") { usage(); return 0; }
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
        config.systemPrompt = readFile(promptPath);
        const bool verbose = config.verbose;
        config.progress = [](std::string_view stage) { std::cerr << stage << std::endl; };
        CliOutput output;
        firefly::RkllmEngine llm(std::move(config));
        firefly::CommandEngine engine(llm);
        std::cerr << "READY" << std::endl;
        const bool interactive = isatty(STDIN_FILENO);
        std::string line;
        while (true) {
            if (interactive) output.write("> ");
            if (!std::getline(std::cin, line)) break;
            const auto first = line.find_first_not_of(" \t\r\n\f\v");
            if (first == std::string::npos) continue;
            const auto last = line.find_last_not_of(" \t\r\n\f\v");
            const auto control = line.substr(first, last - first + 1);
            if (control == "quit" || control == "exit") break;
            const auto start = std::chrono::steady_clock::now();
            const auto result = engine.process(line);
            output.write(result.compact() + "\n");
            if (verbose) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::cerr << (result.path == firefly::CommandPath::FastPath ? "FAST PATH" : "LLM FALLBACK")
                          << " final=" << result.compact() << " command_ms=" << ms << '\n';
            }
        }
        if (std::cin.bad()) throw std::runtime_error("Failed reading stdin");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
