#include "TypedRuntimeChecks.h"
#include <filesystem>
#include <unistd.h>
using namespace runtime_test;
int main(int argc, char** argv) { try {
    if (argc != 2) throw std::runtime_error("root required");
    const runtime::RuntimeConfig owner(std::string(argv[1]) + "/config/current-m4.json");
    check(owner.domain().snapshot()->configuration().schemaVersion == 1, "schema unchanged");
    check(owner.domain().snapshot() == owner.domain().snapshot(), "one retained immutable snapshot");
    std::ostringstream diagnostic; owner.writeDiagnostics(diagnostic);
    check(diagnostic.str().find("action_model=generic domain_authority=config") != std::string::npos, "actual authority diagnostic");
    for (const auto& [alias, id] : owner.domain().snapshot()->mentionIndex())
        check(owner.domain().roomMention(alias)->id == id, "all mention identities");
    for (const auto& [alias, id] : owner.domain().snapshot()->locationIndex())
        check(owner.domain().roomLocation(alias)->id == id, "all location identities");
    check(!owner.domain().room("missing") && !owner.domain().device("missing"), "unknown IDs fail closed");
    const auto file = std::filesystem::temp_directory_path() / ("firefly-m7-owner-" + std::to_string(getpid()) + ".json");
    { std::ofstream out(file); out << owner.domain().snapshot()->canonicalJson(); }
    CandidateResult retained;
    {
        const runtime::RuntimeConfig isolated(file.string());
        Runtime command(isolated.domain().snapshot());
        { std::ofstream out(file); out << "invalid"; }
        retained = command.engine.process("на кухне свет 40").action;
        check(action(retained) == light("device_kitchen_light", 40), "file corruption does not mutate live snapshot");
        bool failed=false;try { const runtime::RuntimeConfig invalid(file.string()); } catch (...) { failed=true; }
        check(failed, "next startup rejects invalid configuration");
        std::filesystem::remove(file);
        check(action(command.engine.process("на кухне свет 40").action) == light("device_kitchen_light",40), "no runtime reload after deletion");
    }
    check(action(retained) == light("device_kitchen_light",40), "typed result survives owner destruction");
    bool failed=false;try { const runtime::RuntimeConfig missing(file.string()); } catch (...) { failed=true; }
    check(failed,"missing configuration fails startup without fallback");
    auto cfg=owner.domain().snapshot()->configuration();cfg.schemaVersion=999;invalidConfig(cfg);
    static_assert(!std::is_copy_assignable_v<runtime::RuntimeConfig>);
    static_assert(!std::is_copy_assignable_v<runtime::DomainView>);
    return finish("RuntimeConfig");
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
