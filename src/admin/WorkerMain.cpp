#include "firefly/admin/RuntimeWorker.h"
#include "firefly/RkllmEngine.h"
#include "Ipc.h"
#include "RuntimeModel.h"
#include <iostream>
#include <poll.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

int main(int argc, char** argv) {
    // Vendor stdout is diagnostic; IPC uses only inherited socket fd 3.
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    const pid_t parent = ::getppid();
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != parent) return 1;
    if (argc != 3 || std::string(argv[1]) != "--model") return 2;
    firefly::admin::RuntimeModel model(argv[2]);
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
