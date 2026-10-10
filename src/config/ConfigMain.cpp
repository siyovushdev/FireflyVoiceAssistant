#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigStore.h"
#include "JsonInternal.h"
#ifdef FIREFLY_CONFIG_BUILD_IDENTITY
#include "AdminBuild.h"
#endif
#include <charconv>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace firefly::config;
void usage() {
    std::cout << "Offline only; production configuration is not activated.\n"
        "firefly-config validate|inspect --file PATH\n"
        "firefly-config audit --store DIR\n"
        "firefly-config init --store EXISTING_PRIVATE_DIR --file PATH\n"
        "firefly-config save --store DIR --file PATH --expected-revision N --expected-hash SHA256\n"
        "firefly-config restore --store DIR --revision N --expected-revision N --expected-hash SHA256\n"
        "firefly-config current|revisions|reconcile --store DIR\n";
}
std::int64_t revision(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || text[0] < '1' || text[0] > '9' || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > MaxRevision)
        throw ConfigError("cli_usage", "", "Expected positive decimal revision");
    return value;
}
}
int main(int argc, char** argv) {
    using namespace firefly::config;
    try {
#ifdef FIREFLY_CONFIG_BUILD_IDENTITY
        if (argc == 2 && std::string(argv[1]) == "--build-identity") {
            std::cout << detail::Json{{"head", firefly::admin::BuildHead},
                {"source_sha256", firefly::admin::BuildSource}, {"built_utc", firefly::admin::BuildTime},
                {"dirty", firefly::admin::BuildDirty}, {"admin_assets_sha256", firefly::admin::BuildAssets}}.dump() << '\n';
            return 0;
        }
#endif
        if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
        if (argc < 2) throw ConfigError("cli_usage", "", "A command is required; see --help");
        const std::string command(argv[1]);
        const std::map<std::string, std::set<std::string>> options{
            {"validate", {"--file"}}, {"inspect", {"--file"}}, {"audit", {"--store"}}, {"init", {"--store", "--file"}},
            {"save", {"--store", "--file", "--expected-revision", "--expected-hash"}},
            {"restore", {"--store", "--revision", "--expected-revision", "--expected-hash"}},
            {"current", {"--store"}}, {"revisions", {"--store"}}, {"reconcile", {"--store"}}};
        if (!options.count(command)) throw ConfigError("cli_usage", "", "Unknown command");
        std::map<std::string, std::string> values;
        for (int i = 2; i < argc; i += 2) {
            const std::string key(argv[i]);
            if (i + 1 >= argc || !options.at(command).count(key) || !values.emplace(key, argv[i + 1]).second)
                throw ConfigError("cli_usage", "", "Unknown, repeated or missing option/value");
        }
        if (values.size() != options.at(command).size()) throw ConfigError("cli_usage", "", "Missing required options");
        Catalog result;
        if (command == "audit") {
            std::cout << inspectStore(values.at("--store"));
            return 0;
        }
        if (command == "validate" || command == "inspect") result = ConfigLoader::fromFile(values.at("--file"));
        else {
            ConfigStore store(values.at("--store"));
            if (command == "init") result = store.initialize(ConfigLoader::fromFile(values.at("--file")));
            else if (command == "current") result = store.current();
            else if (command == "reconcile") result = store.reconcile();
            else if (command == "revisions") {
                detail::Json output{{"production_activated", false}, {"revisions", detail::Json::array()}};
                for (const auto& entry : store.revisions()) output["revisions"].push_back({{"revision", entry.revision}, {"content_sha256", entry.hash}});
                std::cout << output.dump(4) << '\n'; return 0;
            } else {
                const auto hash = values.at("--expected-hash");
                if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw ConfigError("cli_usage", "", "Expected lowercase SHA-256");
                ExpectedRevision expected{revision(values.at("--expected-revision")), hash};
                if (command == "save") result = store.save(ConfigLoader::fromFile(values.at("--file")), expected);
                else result = store.restore(revision(values.at("--revision")), expected);
            }
        }
        std::cout << (result ? result->summary() : "{\"production_activated\":false,\"initialized\":false}\n");
        return 0;
    } catch (const ConfigError& error) {
        std::cerr << detail::Json{{"code", error.code}, {"path", error.path}, {"message", error.what()}, {"production_activated", false}}.dump() << '\n';
        return error.code == "cli_usage" ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << detail::Json{{"code", "internal_error"}, {"message", error.what()}, {"production_activated", false}}.dump(-1, ' ', false, detail::Json::error_handler_t::replace) << '\n';
        return 2;
    }
}
