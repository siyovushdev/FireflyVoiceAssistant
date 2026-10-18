#include "firefly/execution/MqttConnectionProbe.h"
#include "MqttLibrary.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef FIREFLY_WITH_MOSQUITTO
#include <mosquitto.h>
#endif

namespace firefly::execution {

namespace detail {
#ifdef FIREFLY_WITH_MOSQUITTO
void ensureMqttLibrary() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] { initialized = mosquitto_lib_init() == MOSQ_ERR_SUCCESS; });
    if (!initialized) throw std::runtime_error("mqtt_library_unavailable");
}
#else
void ensureMqttLibrary() {
    throw std::runtime_error("mqtt_unavailable");
}
#endif
} // namespace detail

namespace {
MqttProbeResult rejected(std::string code) {
    return {false, std::move(code), false, false};
}
#ifdef FIREFLY_WITH_MOSQUITTO
std::atomic<bool> probeInProgress{false};
class ProbeSlot final {
public:
    ProbeSlot() : acquired_(!probeInProgress.exchange(true, std::memory_order_acq_rel)) {}
    ~ProbeSlot() {
        if (acquired_) probeInProgress.store(false, std::memory_order_release);
    }
    bool acquired() const { return acquired_; }
private:
    bool acquired_;
};
struct ProbeEvent {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    bool disconnected = false;
    int connack = -1;
};
void connected(mosquitto*, void* context, int result) noexcept {
    try {
        auto& state = *static_cast<ProbeEvent*>(context);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.finished) {
                state.connack = result;
                state.finished = true;
            }
        }
        state.cv.notify_one();
    } catch (...) {}
}
void disconnected(mosquitto*, void* context, int) noexcept {
    try {
        auto& state = *static_cast<ProbeEvent*>(context);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.disconnected = true;
            state.finished = true;
        }
        state.cv.notify_one();
    } catch (...) {}
}
struct ProbeClient final {
    mosquitto* ptr = nullptr;
    bool started = false;
    explicit ProbeClient(ProbeEvent* event) {
        detail::ensureMqttLibrary();
        ptr = mosquitto_new(nullptr, true, event); // no persistent MQTT session
        if (!ptr) throw std::runtime_error("mqtt_probe_allocation");
    }
    ~ProbeClient() {
        if (!ptr) return;
        if (started) {
            (void)mosquitto_disconnect(ptr);
            (void)mosquitto_loop_stop(ptr, true);
        }
        mosquitto_destroy(ptr);
    }
    ProbeClient(const ProbeClient&) = delete;
    ProbeClient& operator=(const ProbeClient&) = delete;
};
#endif
} // namespace

MqttProbeResult MqttConnectionProbe::testConnection(
    const config::Connection& connection, const MqttCredentials& credentials) {
    if (connection.type != config::ExecutorKind::Mqtt || !connection.enabled ||
        connection.network.host.empty() || connection.network.port < 1 ||
        connection.network.port > 65535 || connection.timeoutMs < 50 ||
        connection.timeoutMs > 10000)
        return rejected("mqtt_probe_invalid_connection");

#ifndef FIREFLY_WITH_MOSQUITTO
    (void)credentials;
    return rejected("mqtt_unavailable");
#else
    // Bound an otherwise untrusted HTTP client to one short-lived network
    // probe at a time. No username/password or topic is returned to Admin.
    ProbeSlot slot;
    if (!slot.acquired()) return rejected("mqtt_probe_busy");
    try {
        // ProbeEvent MUST outlive ProbeClient: callbacks can still arrive
        // during the latter's loop_stop/join cleanup.
        ProbeEvent state;
        ProbeClient client(&state);
        // libmosquitto 2.x uses MQTT v3.1.1 by default, identical to
        // the production publisher/subscriber. No protocol downgrade.
        if (connection.mqtt.tls) {
            // Exactly the production verifier's CA bundle. Do not disable
            // certificate or hostname verification for the probe.
            if (mosquitto_tls_set(client.ptr, nullptr, "/etc/ssl/certs",
                                  nullptr, nullptr, nullptr) != MOSQ_ERR_SUCCESS)
                return rejected("mqtt_probe_tls_configuration");
            if (mosquitto_tls_insecure_set(client.ptr, false) != MOSQ_ERR_SUCCESS)
                return rejected("mqtt_probe_tls_configuration");
        }
        const auto login = credentials.read(connection.mqtt.credentialsId);
        if (!login.user.empty() &&
            mosquitto_username_pw_set(client.ptr, login.user.c_str(),
                                       login.password.c_str()) != MOSQ_ERR_SUCCESS)
            return rejected("mqtt_probe_credentials_configuration");
        mosquitto_connect_callback_set(client.ptr, connected);
        mosquitto_disconnect_callback_set(client.ptr, disconnected);

        // CONNECT only. Never register an MQTT PUBLISH/SUBSCRIBE callback,
        // subscribe to a topic, or attach this client to RuntimeGeneration.
        if (mosquitto_connect_async(client.ptr, connection.network.host.c_str(),
                                    static_cast<int>(connection.network.port), 30) != MOSQ_ERR_SUCCESS)
            return rejected("mqtt_probe_connection_failed");
        if (mosquitto_loop_start(client.ptr) != MOSQ_ERR_SUCCESS)
            return rejected("mqtt_probe_connection_failed");
        client.started = true;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(connection.timeoutMs);
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!state.cv.wait_until(lock, deadline, [&] { return state.finished; }))
            return rejected("mqtt_probe_timeout");
        const bool disconnectedBeforeResult = state.disconnected;
        const int connack = state.connack;
        lock.unlock();
        if (disconnectedBeforeResult && connack != 0)
            return rejected("mqtt_probe_connection_failed");
        if (connack == 4 || connack == 5)
            return rejected("mqtt_probe_auth_rejected");
        if (connack != 0)
            return rejected(connack == -1 ? "mqtt_probe_connection_failed" :
                             "mqtt_probe_connack_rejected");
        // Not a proof of a working relay: only MQTT CONNECT/CONNACK succeeded.
        return {true, {}, connection.mqtt.tls, !login.user.empty()};
    } catch (const std::exception& error) {
        const std::string code(error.what());
        // MqttCredentials throws only fixed error codes, not secret data.
        if (code.rfind("mqtt_credentials_", 0) == 0) return rejected(code);
        return rejected("mqtt_probe_unavailable");
    }
#endif
}
} // namespace firefly::execution
