#include "firefly/voice/AminaWakeDetector.h"
#include <array>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
using namespace firefly::voice;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        require(argc == 3, "python and fake worker required");
        for (const auto* mode : {"success", "startup-failure", "hang", "disconnect", "malformed"}) {
            bool failed = false;
            const auto start = std::chrono::steady_clock::now();
            try {
                WakeConfig config; config.modelPath = mode;
                AminaWakeDetector wake(config, argv[1], argv[2]);
                wake.reset();
                std::array<float, 512> pcm{};
                require(wake.accept(pcm.data(), pcm.size()), "Expected wake event");
            } catch (const std::exception&) { failed = true; }
            require(failed == (std::string(mode) != "success"), "IPC failure handling incorrect");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "Unbounded worker teardown");
            require(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD, "Leaked worker/zombie");
        }
        std::cout << "Wake IPC deadlines, failures, and child reaping passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
