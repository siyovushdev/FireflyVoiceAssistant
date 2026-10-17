#include "Support.h"
#include "firefly/execution/MqttCredentials.h"
#include "firefly/config/ConfigValidator.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace firefly;
using namespace admin_test;
namespace {
int checks = 0;
void check(bool ok, const char* why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
template<class Callable> void rejects(Callable&& action, const char* reason) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, reason);
}
Json answer(admin::Api& api, const char* method, const char* route, const Json& request,
            int expectedStatus) {
    const auto result = api.handle(method, route, std::string(method) == "GET" ? "" : request.dump());
    check(result.status == expectedStatus, "HTTP status mismatch");
    return Json::parse(result.body);
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        Directory workspace;
        const auto privateDir = workspace.path + "/secure";
        std::filesystem::create_directory(privateDir);
        check(::chmod(privateDir.c_str(), 0700) == 0, "private directory");
        execution::MqttCredentials secrets(privateDir);

        rejects([&] { secrets.read("../secret"); }, "path traversal refused");
        rejects([&] { secrets.create("Bad-ID", "u", "p"); }, "malformed ID refused");
        rejects([&] { secrets.create("newline", "u\nadmin", "p"); }, "newline username refused");
        rejects([&] { secrets.create("badpwd", "u", "p\nsecond"); }, "multiline password refused");
        secrets.create("relay_secret", "broker_user", "private-password-1");
        check(secrets.ids() == std::vector<std::string>{"relay_secret"}, "only private ID listed");
        check(secrets.read("relay_secret").user == "broker_user" &&
              secrets.read("relay_secret").password == "private-password-1",
              "secret read roundtrip");
        struct stat metadata{};
        check(::stat((privateDir + "/relay_secret.cred").c_str(), &metadata) == 0 &&
              (metadata.st_mode & 0777) == 0600 && metadata.st_uid == ::geteuid(),
              "private credentials file mode and owner");
        rejects([&] { secrets.create("relay_secret", "h", "replace"); },
                "create must never replace");
        secrets.rotate("relay_secret", "broker_user_2", "private-password-2");
        check(secrets.read("relay_secret").password == "private-password-2",
              "atomic rotate immediately readable");
        rejects([&] { secrets.rotate("missing_id", "user", "password"); },
                "rotate requires existing secret");
        // Backward-compatible, operator-provisioned read-only secret files
        // remain readable; only new/rotated files need write permission.
        check(::chmod((privateDir + "/relay_secret.cred").c_str(), 0400) == 0,
              "legacy private read-only secret mode");
        check(secrets.read("relay_secret").user == "broker_user_2",
              "old 0400 credential remains usable");
        check(::chmod((privateDir + "/relay_secret.cred").c_str(), 0600) == 0,
              "restore writable mode for rotation tests");

        const auto loose = privateDir + "/loose.cred";
        { std::ofstream out(loose); out << "user\npassword\n"; }
        check(::chmod(loose.c_str(), 0644) == 0, "make wrong-mode fixture");
        rejects([&] { secrets.read("loose"); }, "world/group-readable file refused");
        const auto link = privateDir + "/linked.cred";
        check(::symlink((privateDir + "/relay_secret.cred").c_str(), link.c_str()) == 0,
              "make symlink fixture");
        rejects([&] { secrets.read("linked"); }, "credential symlink refused");
        const auto hardlink = privateDir + "/hardlinked.cred";
        check(::link((privateDir + "/relay_secret.cred").c_str(), hardlink.c_str()) == 0,
              "make hardlink fixture");
        rejects([&] { secrets.read("hardlinked"); }, "credential hardlink refused");
        ::unlink(hardlink.c_str());
        const auto parentLink = workspace.path + "/symlinked_dir";
        check(::symlink(privateDir.c_str(), parentLink.c_str()) == 0,
              "make parent symlink");
        rejects([&] { execution::MqttCredentials(parentLink).ids(); },
                "directory component symlink refused");
        check(::chmod(privateDir.c_str(), 0755) == 0, "make world-readable directory");
        rejects([&] { secrets.read("relay_secret"); }, "unsafe private directory refused");
        check(::chmod(privateDir.c_str(), 0700) == 0, "restore private directory");

        // G2-R4: opening a local FIFO must not block before fstat can reject
        // it. Both file read and Admin rotation use the exact same reader.
        const auto pipePath = privateDir + "/fifo.cred";
        check(::mkfifo(pipePath.c_str(), 0600) == 0, "create corrupt FIFO secret");
        auto fifoStarted = std::chrono::steady_clock::now();
        rejects([&] { (void)secrets.read("fifo"); },
                "FIFO rejected without waiting for a writer");
        check(std::chrono::steady_clock::now() - fifoStarted < std::chrono::seconds(2),
              "FIFO read must not block");
        fifoStarted = std::chrono::steady_clock::now();
        rejects([&] { secrets.rotate("fifo", "user", "password"); },
                "FIFO rotation rejected without waiting for a writer");
        check(std::chrono::steady_clock::now() - fifoStarted < std::chrono::seconds(2),
              "FIFO rotation must not block");
        check(::unlink(pipePath.c_str()) == 0, "remove corrupt FIFO fixture");

        // G2-R3: runtime and Admin own separate store instances. Atomic
        // rename may unlink the *old* inode between openat and fstat, but
        // it remains a complete/validated readable snapshot via its fd.
        execution::MqttCredentials concurrentReader(privateDir);
        secrets.create("raced", "user_a", "password_a");
        std::atomic<bool> rotating{true};
        std::atomic<int> snapshots{0};
        std::string readFailure;
        std::thread reading([&] {
            try {
                while (rotating.load(std::memory_order_acquire)) {
                    const auto login = concurrentReader.read("raced");
                    check((login.user == "user_a" && login.password == "password_a") ||
                          (login.user == "user_b" && login.password == "password_b"),
                          "atomic credential snapshot is neither old nor new");
                    snapshots.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception& error) { readFailure = error.what(); }
        });
        try {
            for (int i = 0; i < 300; ++i) {
                if ((i % 2) == 0) secrets.rotate("raced", "user_b", "password_b");
                else secrets.rotate("raced", "user_a", "password_a");
            }
        } catch (...) {
            rotating.store(false, std::memory_order_release);
            reading.join();
            throw;
        }
        rotating.store(false, std::memory_order_release);
        reading.join();
        check(readFailure.empty() && snapshots.load() > 0,
              "concurrent rotation must never produce a transient credential error");
        check(concurrentReader.read("raced").user == "user_a",
              "final credential after 300 rotations is readable");
        secrets.erase("raced");

        const auto baseV2 = config::ConfigValidator::validate(
            config::migrateV2(catalog(base(argv[1]))->configuration()));
        const auto storeDir = workspace.path + "/store";
        std::filesystem::create_directory(storeDir);
        check(::chmod(storeDir.c_str(), 0700) == 0, "private ConfigStore test directory");
        config::ConfigStore store(storeDir);
        store.initialize(baseV2);
        Model model;
        Client runtime(model);
        admin::Api api(store, runtime, nullptr, privateDir);
        auto listed = answer(api, "GET", "/api/mqtt/credentials", Json::object(), 200);
        check(listed.at("ids").size() == 1 && listed.at("ids").at(0) == "relay_secret",
              "list only valid private IDs, not malformed files");
        const Json create{{"id","other_secret"},{"username","another"},{"password","opaque-web-password"}};
        const auto created = answer(api, "POST", "/api/mqtt/credentials", create, 200);
        check(created.value("ok", false) && created.value("operation", "") == "created" &&
              !created.contains("password") && !created.contains("username") &&
              created.dump().find("opaque-web-password") == std::string::npos,
              "create does not echo secrets");
        check(secrets.read("other_secret").password == "opaque-web-password",
              "Admin created private credential entry");
        const Json rotate{{"id","other_secret"},{"username","another"},{"password","new-web-password"}};
        auto changed = answer(api, "PUT", "/api/mqtt/credentials", rotate, 200);
        check(changed.value("operation","") == "rotated" &&
              changed.dump().find("new-web-password") == std::string::npos,
              "rotate does not echo password");
        check(secrets.read("other_secret").password == "new-web-password",
              "Admin rotates private credential");
        check(answer(api, "DELETE", "/api/mqtt/credentials",
                     Json{{"id","other_secret"}}, 200).value("operation","") == "deleted",
              "unused credential can be deleted");
        rejects([&] { secrets.read("other_secret"); }, "deleted credential absent");

        // A referenced secret is protected in stored revisions/backups and LKG.
        auto configured = Json::parse(baseV2->canonicalJson());
        configured["connections"] = Json::array({Json{
            {"id","connection_secret_ref"},{"name","Protected broker"},{"type","mqtt"},
            {"enabled",true},{"timeout_ms",2000},
            {"network",Json{{"host","127.0.0.1"},{"port",1883}}},
            {"mqtt",Json{{"tls",false},{"credentials_id","relay_secret"}}}
        }});
        const auto current = store.current();
        const auto saved = store.save(config::ConfigLoader::fromJson(configured.dump()),
            {current->configuration().revision, current->contentHash()});
        (void)saved;
        auto inUse = answer(api, "DELETE", "/api/mqtt/credentials",
                            Json{{"id","relay_secret"}}, 409);
        check(inUse.at("errors").dump().find("private-password") == std::string::npos,
              "in-use refusal does not leak credentials");
        // Removing the active reference from the next saved configuration
        // does not invalidate durable backup/LKG snapshots.
        auto unlinked = configured;
        unlinked["connections"] = Json::array();
        const auto inUseCurrent = store.current();
        store.save(config::ConfigLoader::fromJson(unlinked.dump()),
                   {inUseCurrent->configuration().revision, inUseCurrent->contentHash()});
        check(answer(api, "DELETE", "/api/mqtt/credentials",
                     Json{{"id","relay_secret"}}, 409).value("ok", true) == false,
              "historical revision still protects referenced secret");

        // Direct reference verification must work in both MQTT=OFF and ON
        // native builds. MQTT=OFF correctly rejects an enabled MQTT runtime
        // before the Admin preflight, independent of credential readiness.
        secrets.verifyReferences(config::ConfigLoader::fromJson(configured.dump()));
#ifdef FIREFLY_G2_MQTT_AVAILABLE
        const auto valid = answer(api, "POST", "/api/config/validate",
                                  Json{{"configuration",configured}}, 200);
        check(valid.value("ok",false), "valid secret reference accepted");
#endif
        configured["connections"][0]["mqtt"]["credentials_id"] = "missing_reference";
        rejects([&] {
            secrets.verifyReferences(config::ConfigLoader::fromJson(configured.dump()));
        }, "missing credential reference refused by shared reader");
#ifdef FIREFLY_G2_MQTT_AVAILABLE
        const auto invalid = api.handle("POST", "/api/config/validate",
                                        Json{{"configuration",configured}}.dump());
        check(invalid.status == 422 && invalid.body.find("private-password") == std::string::npos,
              "missing reference is fail-closed at Admin Validate");
#endif
        // Configuration export and ordinary Admin GET never expose secret bytes.
        for (const auto* route : {"/api/config", "/api/config/export", "/api/mqtt/credentials"}) {
            const auto response = api.handle("GET", route, "");
            check(response.body.find("private-password") == std::string::npos &&
                  response.body.find("new-web-password") == std::string::npos,
                  "read endpoints redact secrets");
        }
        std::cout << "PASS MQTT credential lifecycle, API and redaction checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MQTT credential test failed: " << error.what() << '\n';
        return 1;
    }
}
