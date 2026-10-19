// Isolated CPU fixture. The parent relays model calls to a real local Qwen.
// It never loads RKLLM, accesses hardware or installs files/services.
#include "../admin/Support.h"
#include "HttpServer.h"
#include <csignal>
#include <iostream>

using admin_test::Json;
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
class RelayModel final : public firefly::admin::ContextModel {
    std::string prompt_, metrics_ = "{}";
    int budget_ = 32;
    std::string call(std::string_view text, std::string_view schema = {}) {
        std::cout << Json{{"event","model_request"},{"system",prompt_},{"text",text},
            {"schema",schema.empty() ? Json(nullptr) : Json::parse(schema)}, {"budget",budget_}}.dump() << std::endl;
        std::string line;
        if (!std::getline(std::cin, line)) throw std::runtime_error("Qwen relay closed");
        const auto reply = Json::parse(line);
        if (reply.contains("error")) throw std::runtime_error(reply.at("error").get<std::string>());
        metrics_ = reply.at("metrics").dump();
        return reply.at("raw").get<std::string>();
    }
public:
    firefly::llm::CommandProtocol commandProtocol() const override { return firefly::llm::CommandProtocol::StableCompact; }
    void useContext(const std::string& prompt) override { prompt_ = prompt; budget_ = 32; }
    void useAdminContext(const std::string& prompt, int budget) override { prompt_ = prompt; budget_ = budget; }
    bool ready() const override { return !prompt_.empty(); }
    std::string infer(std::string_view text) override { return call(text); }
    std::string inferCommand(std::string_view text, std::string_view schema) override { return call(text, schema); }
    std::string inferenceMetrics() const override { return metrics_; }
};
}
int main(int argc, char** argv) {
    if (argc != 4) return 2; // private store, private credentials, fixture config
    try {
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        firefly::config::ConfigStore store(argv[1]);
        store.initialize(firefly::config::ConfigLoader::fromFile(argv[3]));
        RelayModel model;
        firefly::admin::RuntimeWorker worker(model);
        firefly::admin::LocalRuntimeClient runtime(worker);
        firefly::admin::Api api(store, runtime, nullptr, argv[2]);
        api.initializeRuntime();
        firefly::admin::HttpServer server(api, "127.0.0.1", 0);
        std::cout << Json{{"event","listen"},{"url","http://127.0.0.1:" + std::to_string(server.port())}}.dump() << std::endl;
        server.run(stopped);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Power intent fixture: " << error.what() << '\n';
        return 1;
    }
}
