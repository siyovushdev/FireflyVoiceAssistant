#include "HttpServer.h"
#include "firefly/admin/NetworkPolicy.h"
#include "Ipc.h"
#include "firefly/config/ConfigLoader.h"
#include <charconv>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
class Unavailable final : public firefly::admin::RuntimeClient {
    std::string call(std::string_view) override { throw std::runtime_error("No inference worker configured"); }
};
}
int main(int argc, char** argv) {
    try {
        std::string storePath, initial, bind = "127.0.0.1", model, worker;
        unsigned short port = 8080;
        for (int i = 1; i < argc; ++i) {
            const std::string flag(argv[i]);
            if (flag == "--help") {
                std::cout << "firefly-admin --store DIR [--init-config FILE] [--model RKLLM_FILE]\n"
                             "  [--admin-bind 127.0.0.1] [--admin-port 8080] [--runtime-worker PATH]\n"
                             "DIR must exist and be private/user-owned (chmod 700). No model: editor only.\n"
                             "V1 Admin is loopback-only (127.0.0.1); remote access requires an SSH/Tailscale TCP tunnel.\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("Missing option value");
            const std::string value(argv[i]);
            if (flag == "--store") storePath = value;
            else if (flag == "--init-config") initial = value;
            else if (flag == "--admin-bind") bind = value;
            else if (flag == "--model") model = value;
            else if (flag == "--runtime-worker") worker = value;
            else if (flag == "--admin-port") {
                unsigned number;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number > 65535) throw std::invalid_argument("Invalid admin port");
                port = static_cast<unsigned short>(number);
            } else throw std::invalid_argument("Unknown option: " + flag);
        }
        firefly::admin::validateAdminBind(bind);
        if (storePath.empty()) throw std::invalid_argument("--store is required");
        firefly::config::ConfigStore store(storePath);
        if (!initial.empty() && !std::filesystem::exists(std::filesystem::symlink_status(std::filesystem::path(storePath) / "configuration.json")))
            store.initialize(firefly::config::ConfigLoader::fromFile(initial));
        // Broken/recovery-required stores remain inspectable via status/reconcile.
        std::unique_ptr<firefly::admin::RuntimeClient> runtime;
        if (!model.empty() || !worker.empty()) {
            if (worker.empty()) worker = (std::filesystem::canonical("/proc/self/exe").parent_path() / "firefly-admin-runtime").string();
            try { runtime = std::make_unique<firefly::admin::ChildRuntime>(worker, model); }
            catch (const std::exception& error) {
                std::cerr << "Inference worker unavailable; starting configuration editor: " << error.what() << '\n';
                runtime = std::make_unique<Unavailable>();
            }
        } else runtime = std::make_unique<Unavailable>();
        firefly::admin::Api api(store, *runtime);
        firefly::admin::HttpServer server(api, bind, port);
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        std::cout << "ADMIN_LISTEN http://" << bind << ':' << server.port() << '\n' << std::flush;
        std::thread initialize([&api] { api.initializeRuntime(); });
        try { server.run(stopping); }
        catch (...) { initialize.join(); throw; }
        initialize.join();
    } catch (const std::exception& error) { std::cerr << "Admin startup: " << error.what() << '\n'; return 1; }
}
