#include "../admin/Support.h"
#include "SetupSession.h"
#include <iostream>

using admin_test::Json;
namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
// Inference seam only; proposal validation, private draft, Cancel/CAS/Apply/LKG
// and runtime generations are the real product implementations.
class Model final : public admin_test::Model {
public:
    bool opposite = false;
    bool setupRequired() const override { return true; }
    void enterSetup() override {}
    void leaveSetup(const std::string& value) override { useContext(value); }
    void useAdminContext(const std::string& value, int) override { prompt = value; }
    std::string inferAdmin(std::string_view) override {
        ++inferences;
        if (prompt.rfind("Предложи только небольшой русский языковой профиль", 0) != 0)
            return R"({"ops":[["location","Кухня"],["power_device","Кухня","Свет"]]})";
        const auto marker = prompt.rfind("DATA: ");
        check(marker != std::string::npos, "language profile DATA missing");
        const auto data = Json::parse(prompt.substr(marker + 6));
        if (data.contains("assignments")) {
            if (data.at("assignments")[0].at("value").get<bool>())
                return R"({"aliases":["включи","включите","вруби","врубай"]})";
            return opposite ? R"({"aliases":["врубай"]})" :
                R"({"aliases":["выключи","выключите","выключить","отключи","отключите","отключить","выруби","вырубай"]})";
        }
        if (data.contains("speech")) return R"({"aliases":["кухня"],"mentions":["кухня"],"locations":["на кухне"]})";
        return R"({"aliases":["свет"]})";
    }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        for (int scenario = 0; scenario != 3; ++scenario) {
            admin_test::Directory d;
            firefly::config::ConfigStore store(d.path);
            const auto original = store.initialize(firefly::config::ConfigLoader::fromFile(std::string(argv[1]) + "/config/empty-home.json"));
            Model model; model.opposite = scenario == 2;
            firefly::admin::RuntimeWorker worker(model);
            const auto call = [&](const Json& q) { return Json::parse(worker.handle(q.dump())); };
            check(call({{"method","apply"},{"configuration",Json::parse(original->canonicalJson())}}).value("ok",false), "initial runtime");
            const auto originalActive = Json::parse(worker.observe()).at("active");
            firefly::admin::SetupSession setup(store, call);
            auto view = setup.handle("start", {});
            const auto token = view.at("session_id");
            view = setup.handle("message", {{"session_id",token},{"version",view.at("version")},
                {"request_id","create-1"},{"atomic_message",true},
                {"text","Создай зону Кухня и свет на кухне, который можно включать и выключать"}});
            check(view.at("status") == "editing", "automatic setup completed");
            bool foundOff = false;
            for (const auto& a : view.at("configuration").at("actions")) {
                if (a.at("assignments")[0].at("value").get<bool>()) continue;
                foundOff = true;
                const auto aliases = a.at("speech_aliases").get<std::vector<std::string>>();
                if (model.opposite) {
                    check(std::find(aliases.begin(),aliases.end(),"врубай") == aliases.end(), "opposite model phrase admitted");
                    check(!view.value("language_warning",std::string{}).empty(), "rejected enrichment warning missing");
                } else {
                    check(std::find(aliases.begin(),aliases.end(),"отключи") != aliases.end() &&
                          std::find(aliases.begin(),aliases.end(),"вырубай") != aliases.end(), "automatic semantic synonyms missing");
                    check(std::find(aliases.begin(),aliases.end(),"выключить") != aliases.end(), "canonical alias erased");
                }
            }
            check(foundOff && model.inferences >= 5, "new actions actually enriched");
            check(store.current()->contentHash() == original->contentHash(), "draft mutated store before Apply");
            check(Json::parse(worker.observe()).at("active") == originalActive, "draft changed active generation");
            check(Json::parse(worker.observe()).at("executions").empty(), "draft executed device");
            if (scenario == 1) {
                view = setup.handle("apply", {{"session_id",token},{"version",view.at("version")}});
                check(view.at("status") == "applied" && store.current()->contentHash() != original->contentHash(), "explicit Apply failed");
                const auto active = Json::parse(worker.observe()).at("active");
                check(active.at("hash") == store.current()->contentHash(), "active/stored mismatch");
                check(store.lastKnownGood().value()->contentHash() == store.current()->contentHash(), "LKG not published");
                check(Json::parse(worker.observe()).at("executions").empty(), "Apply executed device");
            } else {
                setup.handle("cancel", {{"session_id",token}});
                check(store.current()->contentHash() == original->contentHash(), "Cancel changed store");
                check(Json::parse(worker.observe()).at("active") == originalActive, "Cancel changed active generation");
            }
        }
        std::cout << "PASS AI synonym enrichment: automatic draft, preservation, Cancel/Apply/LKG, opposite phrase rejection, zero execution\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
