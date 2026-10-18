#include "Support.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/execution/MqttConnectionProbe.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace firefly;
using namespace admin_test;

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct FakeConnackBroker final {
    int listener = -1;
    unsigned port = 0;
    int resultCode = 0;
    std::atomic<int> connects{0};
    std::atomic<int> publishes{0};
    std::atomic<int> subscribes{0};
    std::thread worker;

    explicit FakeConnackBroker(int code = 0) : resultCode(code) {
        listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0) throw std::runtime_error("listener_socket");
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = 0;
        bindAddr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) ||
            ::listen(listener, 2)) throw std::runtime_error("listener_bind");
        socklen_t len = sizeof(bindAddr);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bindAddr), &len))
            throw std::runtime_error("listener_port");
        port = ::ntohs(bindAddr.sin_port);
        worker = std::thread([this] {
            pollfd pfd{listener, POLLIN, 0};
            if (::poll(&pfd, 1, 4500) <= 0 || !(pfd.revents & POLLIN)) return;
            const int fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) return;
            timeval timeLimit{2, 0};
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeLimit, sizeof(timeLimit));
            unsigned char first = 0;
            const auto n = ::recv(fd, &first, 1, MSG_WAITALL);
            if (n == 1 && (first & 0xf0) == 0x10) {
                // Read the *complete* MQTT CONNECT frame before responding.
                // TCP may split header/payload over multiple recv() calls;
                // replying to an incomplete frame makes negative tests flaky.
                unsigned remaining = 0, factor = 1;
                bool validLength = false;
                for (int i = 0; i < 4; ++i) {
                    unsigned char byte = 0;
                    if (::recv(fd, &byte, 1, MSG_WAITALL) != 1) break;
                    remaining += (byte & 0x7f) * factor;
                    factor *= 128;
                    if (!(byte & 0x80)) { validLength = true; break; }
                }
                if (validLength && remaining <= 4096) {
                    std::array<unsigned char, 256> buffer{};
                    bool complete = true;
                    while (remaining) {
                        const auto count = remaining < buffer.size() ?
                                           remaining : static_cast<unsigned>(buffer.size());
                        const auto got = ::recv(fd, buffer.data(), count, MSG_WAITALL);
                        if (got <= 0) { complete = false; break; }
                        remaining -= static_cast<unsigned>(got);
                    }
                    if (complete) ++connects;
                }
            }
            // This is intentionally NOT a broker authentication test.
            // For a plaintext connection return CONNACK after the full CONNECT.
            // For a TLS ClientHello return invalid bytes, proving fail closed.
            const unsigned char connack[]{0x20,0x02,0x00,
                                           static_cast<unsigned char>(resultCode)};
            if (n > 0) (void)::send(fd, connack, sizeof(connack), MSG_NOSIGNAL);
            // Wait briefly for any unexpected MQTT packet; DISCONNECT is okay.
            pfd = {fd, POLLIN, 0};
            if (::poll(&pfd, 1, 600) > 0 && (pfd.revents & POLLIN)) {
                std::array<unsigned char, 128> extra{};
                const auto extraN = ::recv(fd, extra.data(), extra.size(), 0);
                if (extraN > 0) {
                    const auto kind = static_cast<unsigned char>(extra[0] & 0xf0);
                    if (kind == 0x30) ++publishes;
                    if (kind == 0x80) ++subscribes;
                }
            }
            (void)::close(fd);
        });
    }
    void finish() {
        if (worker.joinable()) worker.join();
    }
    ~FakeConnackBroker() {
        finish();
        if (listener >= 0) (void)::close(listener);
    }
    FakeConnackBroker(const FakeConnackBroker&) = delete;
    FakeConnackBroker& operator=(const FakeConnackBroker&) = delete;
};
Json mqttConnection(unsigned port, std::string credentialId = {}, bool tls = false) {
    Json connection{{"id","probe_broker"},
                    {"name","External test broker"},
                    {"type","mqtt"},
                    {"enabled",true},
                    {"timeout_ms",1800},
                    {"network",{{"host","127.0.0.1"},{"port",port}}},
                    {"mqtt",{{"tls",tls}}}};
    if (!credentialId.empty())
        connection["mqtt"]["credentials_id"] = credentialId;
    return connection;
}
Json postProbe(admin::Api& api, const Json& connection, int expectedStatus) {
    const auto response = api.handle("POST", "/api/connections/probe",
                                     Json{{"connection",connection}}.dump());
    check(response.status == expectedStatus, "probe HTTP status mismatch");
    return Json::parse(response.body);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        Directory workspace;
        config::ConfigStore store(workspace.path);
        store.initialize(catalog(base(argv[1])));
        Model model;
        Client runtime(model);
        const auto credentialsDir = workspace.path + "/mqtt";
        std::filesystem::create_directory(credentialsDir);
        check(::chmod(credentialsDir.c_str(), 0700) == 0, "private credential fixture");
        execution::MqttCredentials credentials(credentialsDir);
        admin::Api api(store, runtime, nullptr, credentialsDir);
        const auto initial = store.current();

        const auto bad = postProbe(api, mqttConnection(1883, "nonexistent_secret"), 422);
        check(!bad.value("ok", true) &&
              bad.dump().find("not-a-real-password") == std::string::npos,
              "missing probe credentials fail closed and do not echo content");

#ifdef FIREFLY_G2_MQTT_AVAILABLE
        check(bad.at("errors").dump().find("mqtt_credentials_missing") != std::string::npos,
              "probe must report missing private credential reference");
        {
            FakeConnackBroker broker;
            const auto accepted = postProbe(api, mqttConnection(broker.port), 200);
            check(accepted.value("ok", false) &&
                  accepted.value("scope", "") == "mqtt_connack_only" &&
                  accepted.value("transport", "") == "mqtt" &&
                  accepted.value("connack_accepted", false),
                  "CONNECT/CONNACK accepted, not just TCP");
            check(!accepted.value("tls_verified", true) &&
                  !accepted.value("credentials_accepted", true) &&
                  !accepted.value("saved", true) && !accepted.value("applied", true) &&
                  !accepted.value("published", true) && !accepted.value("subscribed", true),
                  "probe reports honest handshake scope with no mutation");
            // Real libmosquitto may send MQTT DISCONNECT after CONNACK.
            broker.finish();
            check(broker.connects.load() == 1 && broker.publishes.load() == 0 &&
                  broker.subscribes.load() == 0,
                  "successful Admin probe sent exactly CONNECT and no command/subscribe");
        }
        {
            FakeConnackBroker broker(5); // simulated MQTT v3.1.1 auth refusal
            const auto rejected = postProbe(api, mqttConnection(broker.port), 422);
            check(!rejected.value("ok", true) &&
                  rejected.at("errors").dump().find("mqtt_probe_auth_rejected") != std::string::npos,
                  "MQTT negative CONNACK must not count as accepted credentials");
            broker.finish();
            check(broker.connects.load() == 1 && broker.publishes.load() == 0 &&
                  broker.subscribes.load() == 0,
                  "negative CONNACK still causes zero MQTT commands and subscriptions");
        }
        credentials.create("isolated_user","broker_user","not-a-real-password");
        {
            FakeConnackBroker broker;
            const auto accepted = postProbe(api, mqttConnection(broker.port,"isolated_user"), 200);
            check(accepted.value("credentials_accepted",false) &&
                  accepted.dump().find("broker_user") == std::string::npos &&
                  accepted.dump().find("not-a-real-password") == std::string::npos,
                  "private credential accepted without username/password echo");
            broker.finish();
            check(broker.connects.load() == 1 && broker.publishes.load() == 0 &&
                  broker.subscribes.load() == 0,
                  "credential probe never publishes or subscribes");
        }
        {
            // A plaintext local peer cannot satisfy trusted CA+hostname TLS.
            // Full real-TLS positive/negative is the independent Codex gate.
            FakeConnackBroker plain;
            const auto denied = postProbe(api, mqttConnection(plain.port,"isolated_user",true), 422);
            check(!denied.value("ok",true), "wrong-protocol or invalid TLS must fail closed");
        }
#else
        check(bad.at("errors").dump().find("mqtt_unavailable") != std::string::npos,
              "MQTT=OFF must fail closed without network");
#endif

        const auto after = store.current();
        check(after->contentHash() == initial->contentHash() &&
              after->configuration().revision == initial->configuration().revision,
              "probe cannot mutate ConfigStore");
        const auto live = api.handle("GET","/api/config/active","");
        check(live.status == 200, "Admin remains available");
        std::cout << "PASS independent MQTT CONNECT-only Admin API probe checks="
                  << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MqttProbeTests failure: " << error.what() << '\n';
        return 1;
    }
}
