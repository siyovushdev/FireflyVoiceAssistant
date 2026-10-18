// One permitted command, no background thread, reconnect, or replay. The parent
// kills/reaps this process at its absolute deadline, including blocking libc DNS.
#include "MqttProcess.h"
#include "MqttLibrary.h"
#include "firefly/config/Connectivity.h"
#include <sys/prctl.h>
#include <sys/resource.h>
#ifdef FIREFLY_WITH_MOSQUITTO
#include <mosquitto.h>
#endif
using namespace firefly::execution::detail;
namespace {
#ifdef FIREFLY_WITH_MOSQUITTO
struct Events {
    bool connected = false, finished = false, disconnected = false;
    int acknowledged = -1;
};
void onConnect(mosquitto*, void* data, int code) noexcept {
    auto& e = *static_cast<Events*>(data);
    if (e.finished || e.disconnected) { e.disconnected = true; return; }
    e.connected = code == 0; e.finished = true;
}
void onDisconnect(mosquitto*, void* data, int) noexcept {
    static_cast<Events*>(data)->disconnected = true;
}
void onPublish(mosquitto*, void* data, int mid) noexcept {
    static_cast<Events*>(data)->acknowledged = mid;
}
struct Client {
    mosquitto* value = nullptr;
    ~Client() {
        if (value) {
            // Never flush a queued command during failure/deadline cleanup.
            const int socket = mosquitto_socket(value);
            if (socket >= 0) (void)::shutdown(socket, SHUT_RDWR);
            mosquitto_destroy(value);
        }
    }
};
bool loginSafe(const std::string& text) {
    if (text.size() > 256) return false;
    for (unsigned char ch : text) if (ch < 32 || ch == 127) return false;
    return true;
}
int execute() {
    nlohmann::json request;
    if (!mqttReceive(0, request, MqttClock::now() + std::chrono::seconds(12), 32768) || request.size() != 11)
        return 2;
    if (request.at("parent_pid").get<pid_t>() != ::getppid()) return 2;
    const auto deadline = MqttClock::time_point(std::chrono::nanoseconds(request.at("deadline_ns").get<std::int64_t>()));
    if (deadline > MqttClock::now() + std::chrono::seconds(10)) return 2;
    const auto fail = [&](const char* code) {
        (void)mqttSend(0, {{"stage","failed"},{"code",code}}, deadline);
        return 0;
    };
    if (!mqttRemaining(deadline)) return 0;
    const auto host = request.at("host").get<std::string>();
    const auto port = request.at("port").get<int>();
    const auto user = request.at("username").get<std::string>();
    const auto password = request.at("password").get<std::string>();
    firefly::config::MqttBinding mapping;
    mapping.topic = request.at("topic").get<std::string>();
    mapping.payload = request.at("payload").get<std::string>();
    mapping.qos = request.at("qos").get<int>();
    mapping.retain = request.at("retain").get<bool>();
    if (host.empty() || host.size() > 253 || port < 1 || port > 65535 ||
        !loginSafe(user) || !loginSafe(password) || user.empty() != password.empty()) return 2;
    firefly::config::Connection connection;
    connection.type = firefly::config::ExecutorKind::Mqtt; connection.enabled = true;
    firefly::config::validateMqttBinding(mapping, &connection);
    MqttSharedOrder order(3);
    if (!order.valid()) return 2;
    ensureMqttLibrary();
    Events e;
    Client client;
    client.value = mosquitto_new(nullptr, true, &e);
    if (!client.value) return fail("mqtt_configuration_or_credentials_error");
    if (request.at("tls").get<bool>() &&
        (mosquitto_tls_set(client.value, nullptr, "/etc/ssl/certs", nullptr, nullptr, nullptr) != MOSQ_ERR_SUCCESS ||
         mosquitto_tls_insecure_set(client.value, false) != MOSQ_ERR_SUCCESS))
        return fail("mqtt_configuration_or_credentials_error");
    if (!user.empty() && mosquitto_username_pw_set(client.value, user.c_str(), password.c_str()) != MOSQ_ERR_SUCCESS)
        return fail("mqtt_configuration_or_credentials_error");
    mosquitto_connect_callback_set(client.value, onConnect);
    mosquitto_disconnect_callback_set(client.value, onDisconnect);
    mosquitto_publish_callback_set(client.value, onPublish);
    // The synchronous connect + manual loop API has no reconnect thread.
    // Only this supervised child can block in the resolver. Keep the original
    // hostname so standard TLS CA, SNI and hostname verification remain intact.
    if (mosquitto_connect(client.value, host.c_str(), port, 30) != MOSQ_ERR_SUCCESS)
        return fail("mqtt_connection_failure");
    while (!e.finished && !e.disconnected) {
        const auto remaining = mqttRemaining(deadline);
        if (!remaining) return 0;
        if (mosquitto_loop(client.value, std::min(remaining, 10), 1) != MOSQ_ERR_SUCCESS)
            return fail("mqtt_connection_failure");
    }
    if (!e.connected || e.disconnected) return fail("mqtt_connect_rejected");
    if (!mqttSend(0, {{"stage","connected"}}, deadline)) return 0;
    nlohmann::json permit;
    if (!mqttReceive(0, permit, deadline) || permit != nlohmann::json{{"publish",true}}) return 0;
    if (!mqttRemaining(deadline) || e.disconnected) return 0;
    // This cross-process ticket is the actual API submission barrier. Parent
    // callbacks captured earlier stay old even if IPC scheduling is delayed.
    const auto ticket = order.next();
    if (!mqttRemaining(deadline)) return 0;
    int mid = -1;
    if (mosquitto_publish(client.value, &mid, mapping.topic.c_str(),
            static_cast<int>(mapping.payload.size()), mapping.payload.data(),
            static_cast<int>(mapping.qos), mapping.retain) != MOSQ_ERR_SUCCESS) return 0;
    if (!mqttSend(0, {{"stage","submitted"},{"ticket",ticket}}, deadline)) return 0;
    while (e.acknowledged != mid && !e.disconnected) {
        const auto remaining = mqttRemaining(deadline);
        if (!remaining) return 0;
        if (mosquitto_loop(client.value, std::min(remaining, 10), 1) != MOSQ_ERR_SUCCESS)
            return fail("mqtt_publish_uncertain");
    }
    if (e.disconnected) return fail("mqtt_publish_uncertain");
    if (mqttRemaining(deadline)) (void)mqttSend(0, {{"stage","accepted"}}, deadline);
    return 0;
}
#endif
}
int main(int argc, char**) {
    if (argc != 1) return 2;
    struct rlimit core{0,0};
    struct stat st{};
    if (::setrlimit(RLIMIT_CORE, &core) || ::prctl(PR_SET_PDEATHSIG, SIGKILL) || ::getppid() == 1 ||
        ::fstat(0, &st) || !S_ISSOCK(st.st_mode)) return 2;
    try {
#ifdef FIREFLY_WITH_MOSQUITTO
        return execute();
#else
        return 2;
#endif
    } catch (const std::exception&) { return 2; } // Never serialize secrets/request.
}
