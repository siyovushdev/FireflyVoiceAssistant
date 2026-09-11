#pragma once
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
namespace firefly::cli {
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
inline std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open system prompt: " + path);
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) throw std::runtime_error("Cannot read system prompt: " + path);
    return contents.str();
}
}
