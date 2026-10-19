// CPU-only HttpServer fixture for isolated MQTT acceptance, never installed.
#include "Support.h"
#include "HttpServer.h"
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <mutex>
#include <csignal>

namespace {
std::string auditPath;
std::mutex auditMutex;
void audit(nlohmann::json event) {
    std::lock_guard<std::mutex> lock(auditMutex);
    const int fd = ::open(auditPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const auto line = event.dump() + '\n';
    const auto written = ::write(fd, line.data(), line.size());
    (void)written;
    ::close(fd);
}
} // namespace

// Transparent observations of the real supervisor syscalls. These wrappers
// are linked only into this test executable, not Admin or the probe worker.
extern "C" int __real_posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*,
                                    const posix_spawnattr_t*, char* const[], char* const[]);
extern "C" int __wrap_posix_spawn(pid_t* pid, const char* path,
                                    const posix_spawn_file_actions_t* actions,
                                    const posix_spawnattr_t* attributes,
                                    char* const args[], char* const environment[]) {
    const auto result = __real_posix_spawn(pid, path, actions, attributes, args, environment);
    audit({{"event", "spawn"}, {"result", result}, {"pid", result ? -1 : *pid}});
    return result;
}
extern "C" int __real_kill(pid_t, int);
extern "C" int __wrap_kill(pid_t pid, int signal) {
    const auto result = __real_kill(pid, signal);
    audit({{"event", "kill"}, {"pid", pid}, {"signal", signal}, {"result", result}});
    return result;
}
extern "C" pid_t __real_waitpid(pid_t, int*, int);
extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options) {
    const auto result = __real_waitpid(pid, status, options);
    if (result > 0)
        audit({{"event", "reap"}, {"pid", result},
               {"signal", WIFSIGNALED(*status) ? WTERMSIG(*status) : 0},
               {"exit", WIFEXITED(*status) ? WEXITSTATUS(*status) : -1}});
    return result;
}

int main(int argc, char** argv) {
    if (argc != 5) return 2; // store, credentials, configuration, private audit
    try {
        auditPath = argv[4];
        // A deliberate non-CLOEXEC descriptor which must NOT reach the worker.
        const int sentinel = ::open(auditPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (sentinel < 0 || ::dup2(sentinel, 71) < 0) return 2;
        if (sentinel != 71) ::close(sentinel);
        if (::fcntl(71, F_SETFD, 0) != 0) return 2;
        firefly::config::ConfigStore store(argv[1]);
        store.initialize(firefly::config::ConfigLoader::fromFile(argv[3]));
        admin_test::Model model;
        admin_test::Client runtime(model);
        firefly::admin::Api api(store, runtime, nullptr, argv[2]);
        api.initializeRuntime();
        firefly::admin::HttpServer server(api, "127.0.0.1", 0);
        std::cout << "ADMIN_LISTEN http://127.0.0.1:" << server.port() << std::endl;
        volatile std::sig_atomic_t stop = 0;
        server.run(stop);
    } catch (const std::exception&) {
        std::cerr << "MQTT test HttpServer fixture failed\n";
        return 1;
    }
}
