#include "firefly/admin/RuntimeWorker.h"
#include "firefly/RkllmEngine.h"
#include "Ipc.h"
#include <iostream>
#include <poll.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

namespace {
class Model final : public firefly::admin::ContextModel {
public:
    explicit Model(std::string path) : path_(std::move(path)) {}
    void useContext(const std::string& prompt) override {
        if (path_.empty()) throw std::runtime_error("Model not configured");
        if (!engine_ || !engine_->ready()) {
            // Reinitialization is only recovery after a fatal runtime failure.
            engine_.reset();
            firefly::RkllmConfig config;
            config.modelPath = path_; config.systemPrompt = prompt;
            config.verbose = true; config.requestTokenReserve = 128;
            engine_ = std::make_unique<firefly::RkllmEngine>(std::move(config));
        } else engine_->setSystemPrompt(prompt);
    }
    bool ready() const override { return engine_ && engine_->ready(); }
    std::string infer(std::string_view text) override {
        if (!engine_) throw std::runtime_error("Model unavailable");
        return engine_->infer(text);
    }
private:
    std::string path_;
    std::unique_ptr<firefly::RkllmEngine> engine_;
};
}
int main(int argc, char** argv) {
    // Vendor stdout is diagnostic; IPC uses only inherited socket fd 3.
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    const pid_t parent = ::getppid();
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != parent) return 1;
    if (argc != 3 || std::string(argv[1]) != "--model") return 2;
    Model model(argv[2]);
    firefly::admin::RuntimeWorker worker(model);
    try {
        while (true) {
            pollfd descriptor{3, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
            const auto request = firefly::admin::receiveMessage(3, 5);
            firefly::admin::sendMessage(3, worker.handle(request), 5);
        }
    } catch (const std::exception& error) { std::cerr << "Runtime worker stopped: " << error.what() << '\n'; }
}
