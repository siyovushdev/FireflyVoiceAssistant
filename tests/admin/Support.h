#pragma once
#include "firefly/admin/Api.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/llm/StructuredOutput.h"
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unistd.h>
namespace admin_test {
using Json = nlohmann::json;
inline Json base(const std::string& root) { std::ifstream file(root + "/config/current-m4.json"); return Json::parse(file); }
inline Json bathroom(Json c) {
    c["rooms"].push_back({{"id","room_bathroom"},{"name","Ванная"},{"speech",{{"mentions",{"ванная","ванна","ванной","ванне"}},{"locations",{"в ванной","в ванне"}}}}});
    c["devices"].push_back({{"id","device_bathroom_light"},{"name","Свет в ванной"},{"kind","light"},{"room_id","room_bathroom"},{"enabled",true},{"action_ids",{"action_set_brightness"}},{"spoken_aliases",Json::array()}});
    return c;
}
inline firefly::config::Catalog catalog(const Json& c) { return firefly::config::ConfigLoader::fromJson(c.dump()); }
inline Json typed(const firefly::actions::CandidateResult& value) { return Json::parse(firefly::llm::structuredResult(value)); }
inline Json light(const std::string& target, int brightness) { return {{"actionId","action_set_brightness"},{"targetId",target},{"arguments",{{"brightness",brightness}}}}; }
struct Directory {
    std::string path;
    Directory() { char pattern[] = "/tmp/firefly-admin-tests-XXXXXX"; const auto result = ::mkdtemp(pattern); if (!result) throw std::runtime_error("mkdtemp"); path = result; }
    ~Directory() { std::filesystem::remove_all(path); }
};
// Inference seam only. All parsing, safety, validation, store and generations
// exercised by the tests are production implementations.
class Model : public firefly::admin::ContextModel {
public:
    std::string prompt, raw = R"({"unsupported":true})";
    bool reject = false, available = true;
    int refreshes = 0, inferences = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool block = false, entered = false, release = false;
    void useContext(const std::string& value) override {
        if (!available || reject || value.find("REJECT_CONTEXT") != std::string::npos) throw std::runtime_error("injected admission failure");
        prompt = value; ++refreshes;
    }
    bool ready() const override { return available && !prompt.empty(); }
    std::string infer(std::string_view) override {
        ++inferences;
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        if (block) cv.wait(lock, [&]{return release;});
        return raw;
    }
};
class Client : public firefly::admin::RuntimeClient {
public:
    firefly::admin::RuntimeWorker worker;
    explicit Client(Model& model) : worker(model) {}
    std::string call(std::string_view message) override { return worker.handle(message); }
};
}
