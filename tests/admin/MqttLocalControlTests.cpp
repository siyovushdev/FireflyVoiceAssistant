#include "Support.h"
#include "firefly/admin/LocalMqttBrokerControl.h"
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

        // A socket endpoint owned by a different UID is not a privileged
        // broker controller, even if its bytes mimic the normal protocol.
        auto untrusted = std::make_shared<admin::LocalMqttBrokerControl>(
            socket, static_cast<unsigned>(::geteuid()) + 1);
        admin::Api impersonation(store, runtime, nullptr, credentials, untrusted);
        const auto blocked = answer(impersonation, "GET", "/api/mqtt/local-broker",
                                    Json::object(), 503);
        check(!blocked.value("ok",true), "untrusted peer UID rejected");

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
