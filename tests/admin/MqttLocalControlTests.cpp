#include "Support.h"
#include "SetupSession.h"
#include "firefly/admin/LocalMqttBrokerControl.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace firefly;
using namespace admin_test;

namespace {
// Use the real Api/RuntimeWorker/SetupSession lifecycle with only the vendor
// model entry seam replaced. No privileged broker or ConfigStore is mocked.
struct SetupCapableModel final : Model {
    bool setupRequired() const override { return true; }
    void enterSetup() override {}
    void leaveSetup(const std::string& prompt) override { useContext(prompt); }
};
int checks = 0;
void check(bool correct, const char* message) {
    ++checks;
    if (!correct) throw std::runtime_error(message);
}
Json answer(admin::Api& api, std::string_view method, std::string_view route,
            const Json& body, int status) {
    const auto r = api.handle(method, route, method == "GET" ? "" : body.dump());
    check(r.status == status, "wrong local broker API HTTP status");
    return Json::parse(r.body);
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) return 2;
        const auto socket = std::string(argv[2]);
        auto control = std::make_shared<admin::LocalMqttBrokerControl>(
            socket, static_cast<unsigned>(::geteuid()));
        // A dedicated rootless fake-systemctl helper tests the *real* client
        // and Admin route; no production systemd or broker is touched.
        const auto initial = control->status();
        check(initial.installed && !initial.enabled && !initial.running,
              "default local broker must be installed but OFF");
        Directory dir;
        config::ConfigStore store(dir.path);
        store.initialize(catalog(base(argv[1])));
        Model model;
        Client runtime(model);
        auto credentials = dir.path + "/credentials";
        std::filesystem::create_directory(credentials);
        check(::chmod(credentials.c_str(), 0700) == 0, "private test credentials");
        admin::Api api(store, runtime, nullptr, credentials, control);
        const auto state = answer(api, "GET", "/api/mqtt/local-broker",
                                  Json::object(), 200);
        check(state.value("installed",false) &&
              !state.value("enabled",true) && !state.value("running",true),
              "Admin status reflects broker OFF, not fabricated open-now");
        const auto original = store.current();
        auto deny = answer(api, "POST", "/api/mqtt/local-broker",
                           Json{{"operation","enable"},{"confirm",false}}, 422);
        check(!deny.value("ok",true), "enable must require explicit confirmation");
        deny = answer(api, "POST", "/api/mqtt/local-broker",
                           Json{{"operation","restart"},{"confirm",true}}, 422);
        check(!deny.value("ok",true), "arbitrary operations forbidden by Admin");
        deny = answer(api, "POST", "/api/mqtt/local-broker",
                           Json{{"operation","enable"},{"confirm",true},
                                {"unit","ssh.service"}}, 400);
        check(!deny.value("ok",true), "unit injection rejected by strict members");

        auto enabled = answer(api, "POST", "/api/mqtt/local-broker",
                              Json{{"operation","enable"},{"confirm",true}}, 200);
        check(enabled.value("installed",false) && enabled.value("enabled",false) &&
              enabled.value("running",false),
              "enable must be verified by subsequent systemd status");
        auto seen = answer(api, "GET", "/api/mqtt/local-broker", Json::object(), 200);
        check(seen.value("enabled",false) && seen.value("running",false),
              "enabled status persists across separate requests");
        auto disabled = answer(api, "POST", "/api/mqtt/local-broker",
                               Json{{"operation","disable"},{"confirm",true}}, 200);
        check(!disabled.value("enabled",true) && !disabled.value("running",true),
              "disable must confirm stopped and disabled");
        seen = answer(api, "GET", "/api/mqtt/local-broker", Json::object(), 200);
        check(!seen.value("enabled",true) && !seen.value("running",true),
              "disabled status persists across separate requests");

        // Negative scenario: fixed systemd unit itself is absent.
        // A generic "not installed" must not be confused with a stopped,
        // controllable service or a successful enable.
        const auto fakeState = std::filesystem::path(socket).parent_path() / "state.txt";
        { std::ofstream state(fakeState); state << "not-found stopped\n"; }
        const auto absent = answer(api, "GET", "/api/mqtt/local-broker",
                                   Json::object(), 200);
        check(!absent.value("installed",true), "absent systemd unit visible");
        const auto rejected = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","enable"},{"confirm",true}}, 409);
        check(!rejected.value("ok",true), "cannot enable a missing unit");
        { std::ofstream state(fakeState); state << "disabled stopped\n"; }

        // R1: a unit can remain installed even if Mosquitto was uninstalled.
        // The real helper must not infer "installed" from systemctl alone.
        const auto binary = std::filesystem::path(socket).parent_path() /
                            "mosquitto-test-bin";
        const auto parkedBinary = std::filesystem::path(socket).parent_path() /
                                  "mosquitto-test-bin.parked";
        check(std::filesystem::exists(binary), "test broker binary exists");
        std::filesystem::rename(binary, parkedBinary);
        const auto absentPackage = answer(api, "GET", "/api/mqtt/local-broker",
                                          Json::object(), 200);
        check(!absentPackage.value("installed", true) &&
              !absentPackage.value("running", true) &&
              !absentPackage.value("enabled", true),
              "installed systemd unit without broker binary is NOT installed");
        const auto denyWithoutBinary = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","enable"},{"confirm",true}}, 409);
        check(!denyWithoutBinary.value("ok",true),
              "missing broker binary cannot be enabled");

        // Retain an orphan enabled unit if its binary disappeared; expose
        // enabled state honestly and allow a separate explicit Disable.
        { std::ofstream state(fakeState); state << "enabled stopped\n"; }
        const auto orphan = answer(api, "GET", "/api/mqtt/local-broker",
                                    Json::object(), 200);
        check(!orphan.value("installed", true) &&
              orphan.value("enabled", false) && !orphan.value("running", true),
              "orphan autostart stays visible despite missing binary");
        const auto disableOrphan = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","disable"},{"confirm",true}}, 200);
        check(!disableOrphan.value("enabled",true) &&
              !disableOrphan.value("running",true),
              "explicit Disable can clear orphan autostart safely");

        // New R1 regression: unlinking an executable does not terminate the
        // running service. Keep the running state visible and allow a safe
        // explicit fixed-unit Disable. The fake systemctl only models unit
        // state; independent broker E2E must also test a real unlinked process.
        { std::ofstream state(fakeState); state << "enabled running\n"; }
        const auto orphanRunning = answer(api, "GET", "/api/mqtt/local-broker",
                                          Json::object(), 200);
        check(!orphanRunning.value("installed",true) &&
              orphanRunning.value("enabled",false) &&
              orphanRunning.value("running",false),
              "unlinked binary with existing running process is visible");
        const auto disableRunning = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","disable"},{"confirm",true}}, 200);
        check(!disableRunning.value("enabled",true) &&
              !disableRunning.value("running",true),
              "Disable stops an enabled/running orphan process");
        { std::ofstream state(fakeState); state << "disabled running\n"; }
        const auto manuallyRunning = answer(api, "GET", "/api/mqtt/local-broker",
                                             Json::object(), 200);
        check(!manuallyRunning.value("installed",true) &&
              !manuallyRunning.value("enabled",true) &&
              manuallyRunning.value("running",false),
              "manual running process survives missing executable detection");
        const auto stopManual = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","disable"},{"confirm",true}}, 200);
        check(!stopManual.value("running",true) &&
              !stopManual.value("enabled",true),
              "Disable stops manually started orphan process");
        std::filesystem::rename(parkedBinary, binary);
        const auto restored = answer(api, "GET", "/api/mqtt/local-broker",
                                     Json::object(), 200);
        check(restored.value("installed", false) &&
              !restored.value("enabled", true),
              "reinstalled binary restores accurate installed status");

        // The privileged helper must check the service's actual postcondition.
        // A systemctl command returning success while the broker crashes
        // (e.g. missing password file) must never produce a fake enabled PASS.
        const auto failStart = std::filesystem::path(socket).parent_path() / "fail-enable";
        { std::ofstream mark(failStart); mark << "fail\n"; }
        const auto falseSuccess = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","enable"},{"confirm",true}}, 503);
        check(!falseSuccess.value("ok",true),
              "broker failed to run must not be reported as enabled");
        const auto unready = answer(api, "GET", "/api/mqtt/local-broker",
                                    Json::object(), 200);
        check(!unready.value("enabled",true) && !unready.value("running",true),
              "failed Enable must roll back persistent autostart automatically");
        check(std::filesystem::remove(failStart), "remove fake failure marker");

        // R2: systemctl itself can exit nonzero AFTER changing is-enabled.
        // Compensation is required on both command-failure and failed-
        // postcondition paths; no separate owner Disable should be needed.
        const auto failCommand = std::filesystem::path(socket).parent_path() /
                                 "fail-command";
        { std::ofstream marker(failCommand); marker << "fail\n"; }
        const auto partialEnable = answer(api, "POST", "/api/mqtt/local-broker",
            Json{{"operation","enable"},{"confirm",true}}, 503);
        check(!partialEnable.value("ok",true),
              "failed systemctl enable must never claim success");
        const auto compensated = answer(api, "GET", "/api/mqtt/local-broker",
                                        Json::object(), 200);
        check(compensated.value("installed",false) &&
              !compensated.value("enabled",true) &&
              !compensated.value("running",true),
              "nonzero enable after partial side effect restores disabled/off");
        check(std::filesystem::remove(failCommand), "remove fake command failure");

        // A socket endpoint owned by a different UID is not a privileged
        // broker controller, even if its bytes mimic the normal protocol.
        auto untrusted = std::make_shared<admin::LocalMqttBrokerControl>(
            socket, static_cast<unsigned>(::geteuid()) + 1);
        admin::Api impersonation(store, runtime, nullptr, credentials, untrusted);
        const auto blocked = answer(impersonation, "GET", "/api/mqtt/local-broker",
                                    Json::object(), 503);
        check(!blocked.value("ok",true), "untrusted peer UID rejected");

        // Real Api + RuntimeWorker: once Setup has started, broker mutations
        // are denied, while status reads remain independently available.
        {
            Directory setupDir;
            config::ConfigStore setupStore(setupDir.path);
            const auto baseline = setupStore.initialize(catalog(base(argv[1])));
            SetupCapableModel setupModel;
            Client setupRuntime(setupModel);
            admin::Api setupApi(setupStore, setupRuntime, nullptr, credentials, control);
            setupApi.initializeRuntime();
            const auto startResponse = setupApi.handle("POST", "/api/setup/start", "{}");
            check(startResponse.status == 200, "independent Setup can enter");
            const auto started = Json::parse(startResponse.body);
            check(started.value("active",false) && started.contains("session_id"),
                  "setup ownership published before external broker operations");
            const auto refused = setupApi.handle("POST", "/api/mqtt/local-broker",
                Json{{"operation","enable"},{"confirm",true}}.dump());
            check(refused.status == 409 &&
                  refused.body.find("setup_in_progress") != std::string::npos,
                  "enable denied while AI Setup active");
            const auto statusDuringSetup = setupApi.handle("GET",
                "/api/mqtt/local-broker", "");
            check(statusDuringSetup.status == 200,
                  "broker status remains readable during Setup");
            const auto cancellation = setupApi.handle("POST", "/api/setup/cancel",
                Json{{"session_id",started.at("session_id")}}.dump());
            check(cancellation.status == 200,
                  "setup cancellation remains possible after blocked broker operation");
            check(setupStore.current()->contentHash() == baseline->contentHash(),
                  "setup broker-control refusal cannot mutate ConfigStore");
        }

        // Deterministic admission gate interleaving. A broker operation
        // holding the shared mutex must complete BEFORE Setup becomes active.
        // Once it becomes active, any later broker operation obtaining the
        // same gate must refuse instead of using an earlier stale check.
        {
            Directory gateDir;
            config::ConfigStore gateStore(gateDir.path);
            gateStore.initialize(catalog(base(argv[1])));
            std::mutex gate;
            std::atomic<bool> waitingForGate{false};
            std::atomic<bool> finishedSetup{false};
            std::string entryError;
            admin::SetupSession guardedSetup(gateStore,
                [](const Json&) -> Json { return Json{{"ok",true}}; },
                {},
                [&]() {
                    waitingForGate.store(true, std::memory_order_release);
                    return std::unique_lock<std::mutex>(gate);
                });
            std::unique_lock<std::mutex> heldBrokerOperation(gate);
            std::thread enterSetup([&] {
                try {
                    (void)guardedSetup.handle("start", Json::object());
                    finishedSetup.store(true, std::memory_order_release);
                } catch (const std::exception& e) {
                    entryError = e.what();
                }
            });
            bool waiting = false;
            for (int tries = 0; tries < 2000; ++tries) {
                if (waitingForGate.load(std::memory_order_acquire)) {
                    waiting = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool beforeRelease = !guardedSetup.active() &&
                !finishedSetup.load(std::memory_order_acquire);
            heldBrokerOperation.unlock();
            enterSetup.join();
            check(waiting && beforeRelease && entryError.empty() &&
                  finishedSetup.load(std::memory_order_acquire) &&
                  guardedSetup.active(),
                  "broker mutex gates active Setup admission and ownership transition");
            bool wouldAuthorizeBrokerMutation = false;
            {
                std::lock_guard<std::mutex> gateLock(gate);
                wouldAuthorizeBrokerMutation = !guardedSetup.active();
            }
            check(!wouldAuthorizeBrokerMutation,
                  "broker operation rechecking active under gate rejects later mutation");
        }

        const auto after = store.current();
        check(after->contentHash() == original->contentHash() &&
              after->configuration().revision == original->configuration().revision,
              "control operations must not change ConfigStore or Apply runtime");

        // Exercise an unreachable privileged socket. No fake success is
        // permitted and the private credentials/catalog must remain untouched.
        auto missingControl = std::make_shared<admin::LocalMqttBrokerControl>(
            dir.path + "/missing.sock", static_cast<unsigned>(::geteuid()));
        admin::Api unavailable(store, runtime, nullptr, credentials, missingControl);
        deny = answer(unavailable, "GET", "/api/mqtt/local-broker",
                       Json::object(), 503);
        check(!deny.value("ok",true), "absent controller cannot claim success");
        deny = answer(unavailable, "POST", "/api/mqtt/local-broker",
                      Json{{"operation","enable"},{"confirm",true}}, 503);
        check(!deny.value("ok",true), "absent helper cannot claim enabled");

        std::cout << "PASS local MQTT broker control Admin API checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MqttLocalBrokerControl FAIL: " << error.what() << '\n';
        return 1;
    }
}
