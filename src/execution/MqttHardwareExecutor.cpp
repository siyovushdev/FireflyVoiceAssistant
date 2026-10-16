#include "firefly/execution/MqttHardwareExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include "firefly/config/Connectivity.h"
#include "firefly/actions/ScalarJson.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef FIREFLY_WITH_MOSQUITTO
#include <mosquitto.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace firefly::execution {
std::uint64_t MqttObservationGate::snapshot() const {
    return epoch_.load(std::memory_order_acquire);
}
void MqttObservationGate::begin(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++pending_[target];
    epoch_.fetch_add(1, std::memory_order_release);
}
void MqttObservationGate::end(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(target);
    if (it == pending_.end()) return;
    if (--it->second == 0) pending_.erase(it);
    epoch_.fetch_add(1, std::memory_order_release);
}
bool MqttObservationGate::accept(const std::string& target, std::uint64_t version,
                                 const std::function<void()>& commit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch_.load(std::memory_order_acquire) != version || pending_.count(target))
        return false;
    commit();
    return true;
}
std::map<std::string, actions::Scalar> decodeMqttObservationPayload(
    const config::ObservationBinding& binding, std::string_view payload) {
    if (payload.empty() || payload.size() > 4096)
        throw std::runtime_error("mqtt_invalid_observation_size");
    std::map<std::string, actions::Scalar> result;
    if (binding.mqtt.format == "text") {
        if (payload == binding.mqtt.truePayload) result.emplace(binding.mqtt.field, actions::Scalar(true));
        else if (payload == binding.mqtt.falsePayload) result.emplace(binding.mqtt.field, actions::Scalar(false));
        else throw std::runtime_error("mqtt_unknown_boolean_payload");
        return result;
    }
    if (binding.mqtt.format != "json") throw std::runtime_error("mqtt_unsupported_format");
    // nlohmann's default parser accepts duplicate keys (last value wins).
    // Reject every duplicate, including nested ones, before any state mutation.
    actions::checkNumericLexemes(payload);
    std::map<int, std::set<std::string>> keys;
    const auto json = nlohmann::json::parse(payload.begin(), payload.end(),
        [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
            using Event = nlohmann::json::parse_event_t;
            if ((event == Event::object_start || event == Event::array_start) && depth >= 16)
                throw std::runtime_error("mqtt_json_too_deep");
            if (event == Event::object_start) keys[depth] = {};
            if (event == Event::object_end) keys.erase(depth);
            if (event == Event::key &&
                !keys.at(depth - 1).insert(value.get<std::string>()).second)
                throw std::runtime_error("mqtt_duplicate_json_key");
            return true;
        });
    if (!json.is_object()) throw std::runtime_error("mqtt_state_requires_json_object");
    for (const auto& [source, destination] : binding.fields) {
        if (!json.contains(source)) throw std::runtime_error("mqtt_missing_json_state_field");
        result.emplace(destination, actions::decodeScalar(json.at(source)));
    }
    return result;
}
namespace {
HardwareResult error(std::string code, ReachabilityEvidence evidence = ReachabilityEvidence::None) {
    return {false, std::move(code), {}, evidence};
}
#ifdef FIREFLY_WITH_MOSQUITTO
void initializeLibrary() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] { initialized = mosquitto_lib_init() == MOSQ_ERR_SUCCESS; });
    if (!initialized) throw std::runtime_error("mosquitto library initialization failed");
}
struct Login { std::string user, password; };
Login readLogin(const std::string& id) {
    if (id.empty()) return {};
    // Fixed, operator-provisioned private directory. Never accept a file path from
    // ConfigStore. No symlink, world/group permission or foreign-owned secret.
    const std::string name = "/var/lib/firefly/mqtt/credentials/" + id + ".cred";
    const int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("mqtt_credentials_missing");
    struct Descriptor { int fd; ~Descriptor() { ::close(fd); } } guard{fd};
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 077) != 0 || st.st_size < 3 || st.st_size > 4096)
        throw std::runtime_error("mqtt_credentials_permissions");
    std::string content;
    char bytes[256];
    for (;;) {
        const auto count = ::read(fd, bytes, sizeof(bytes));
        if (count < 0) throw std::runtime_error("mqtt_credentials_read");
        if (count == 0) break;
        content.append(bytes, static_cast<std::size_t>(count));
        if (content.size() > 4096) throw std::runtime_error("mqtt_credentials_size");
    }
    const auto first = content.find('\n');
    if (first == std::string::npos) throw std::runtime_error("mqtt_credentials_format");
    const auto second = content.find('\n', first + 1);
    if (second != content.size() - 1 || first == 0 || first > 256 ||
        second == std::string::npos || second - first < 2 || second - first > 257 ||
        content.find('\0') != std::string::npos || content.find('\r') != std::string::npos)
        throw std::runtime_error("mqtt_credentials_format");
    return {content.substr(0, first), content.substr(first + 1, second - first - 1)};
}
struct Client {
    mosquitto* ptr = nullptr;
    bool running = false;
    explicit Client(void* user) {
        initializeLibrary();
        ptr = mosquitto_new(nullptr, true, user); // unique clean-session ID
        if (!ptr) throw std::runtime_error("mqtt_client_allocation");
    }
    ~Client() {
        if (!ptr) return;
        if (running) { mosquitto_disconnect(ptr); mosquitto_loop_stop(ptr, true); }
        mosquitto_destroy(ptr);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
};
void configure(Client& client, const config::Connection& connection) {
    if (connection.mqtt.tls) {
        // Validate server certificate against system CA roots. Never disable hostname
        // verification or trust self-signed certificates implicitly.
        if (mosquitto_tls_set(client.ptr, nullptr, "/etc/ssl/certs", nullptr, nullptr) != MOSQ_ERR_SUCCESS)
            throw std::runtime_error("mqtt_tls_configuration");
    }
    auto login = readLogin(connection.mqtt.credentialsId);
    if (!login.user.empty() &&
        mosquitto_username_pw_set(client.ptr, login.user.c_str(), login.password.c_str()) != MOSQ_ERR_SUCCESS)
        throw std::runtime_error("mqtt_credentials_rejected");
    // Caller owns the entire client; no credentials retained in ConfigStore/logs.
}
struct Publication {
    bool connected = false, connectionFinished = false;
    int acknowledgedMid = -1;
};
void connected(mosquitto*, void* context, int code) {
    auto& p = *static_cast<Publication*>(context);
    p.connected = code == 0;
    p.connectionFinished = true;
}
void published(mosquitto*, void* context, int id) {
    static_cast<Publication*>(context)->acknowledgedMid = id;
}
HardwareResult publish(const config::Connection& conn, const config::MqttBinding& mapping) {
    Publication p;
    try {
        Client client(&p);
        configure(client, conn);
        mosquitto_connect_callback_set(client.ptr, connected);
        mosquitto_publish_callback_set(client.ptr, published);
        if (mosquitto_connect_async(client.ptr, conn.network.host.c_str(),
                static_cast<int>(conn.network.port), 30) != MOSQ_ERR_SUCCESS)
            return error("mqtt_connection_failure", ReachabilityEvidence::Unreachable);

        // Drive a one-shot client in the calling thread. Unlike loop_start(),
        // mosquitto_loop() does not automatically reconnect and replay an
        // unacknowledged QoS 1 command on network recovery.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(conn.timeoutMs);
        while (!p.connectionFinished && std::chrono::steady_clock::now() < deadline) {
            if (mosquitto_loop(client.ptr, 50, 1) != MOSQ_ERR_SUCCESS)
                return error("mqtt_connection_failure", ReachabilityEvidence::Unreachable);
        }
        if (!p.connectionFinished) return error("mqtt_timeout", ReachabilityEvidence::Unreachable);
        if (!p.connected) return error("mqtt_connect_rejected", ReachabilityEvidence::Unreachable);

        int mid = -1;
        if (mosquitto_publish(client.ptr, &mid, mapping.topic.c_str(),
                static_cast<int>(mapping.payload.size()), mapping.payload.data(),
                static_cast<int>(mapping.qos), mapping.retain) != MOSQ_ERR_SUCCESS)
            return error("mqtt_publish_rejected", ReachabilityEvidence::Reachable);
        while (p.acknowledgedMid != mid && std::chrono::steady_clock::now() < deadline) {
            if (mosquitto_loop(client.ptr, 50, 1) != MOSQ_ERR_SUCCESS)
                return error("mqtt_publish_uncertain", ReachabilityEvidence::Reachable);
        }
        if (p.acknowledgedMid != mid) return error("mqtt_publish_uncertain", ReachabilityEvidence::Reachable);
        // Packet transmitted/PUBACK obtained; this does not assert device state.
        return {true, {}, {}, ReachabilityEvidence::Reachable};
    } catch (const std::exception&) {
        // Never print secrets or broker-provided text in public diagnostics.
        return error("mqtt_configuration_or_credentials_error");
    }
}
#endif
}

struct MqttHardwareExecutor::Impl {
    config::Catalog catalog;
    DeviceStateStore& state;
    MqttObservationGate gate;
#ifdef FIREFLY_WITH_MOSQUITTO
    struct Subscription {
        Impl& owner;
        const config::Connection& connection;
        Client client;
        std::map<std::string, std::vector<const config::ObservationBinding*>> byTopic;
        Subscription(Impl& owner, const config::Connection& c)
            : owner(owner), connection(c), client(this) {}
        ~Subscription() {
            // Join callbacks before byTopic and the referenced DeviceStateStore
            // may be destroyed. Client's later destructor only frees the handle.
            if (client.running) {
                mosquitto_disconnect(client.ptr);
                mosquitto_loop_stop(client.ptr, true);
                client.running = false;
            }
        }

        static void onConnect(mosquitto* client, void* ptr, int code) {
            auto& s = *static_cast<Subscription*>(ptr);
            if (code == 0) {
                for (const auto& [topic, bindings] : s.byTopic) {
                    int qos = 0;
                    for (const auto* binding : bindings) qos = std::max(qos, static_cast<int>(binding->mqtt.qos));
                    (void)mosquitto_subscribe(client, nullptr, topic.c_str(), qos);
                }
            } else s.disconnected();
        }
        static void onDisconnect(mosquitto*, void* ptr, int) {
            static_cast<Subscription*>(ptr)->disconnected();
        }
        void disconnected() {
            for (const auto& [topic, bindings] : byTopic)
                for (const auto* binding : bindings) {
                    owner.state.invalidate(binding->targetId);
                    owner.state.markUnreachable(binding->targetId, owner.state.nowMs());
                }
        }
        static void onMessage(mosquitto*, void* ptr, const mosquitto_message* message) {
            auto& s = *static_cast<Subscription*>(ptr);
            // Capture the generation before doing parsing or waiting on a lock.
            // A command start/end across this callback invalidates this sample.
            const auto observationEpoch = s.owner.gate.snapshot();
            if (!message || !message->topic || message->retain ||
                message->payloadlen < 0 || message->payloadlen > 4096) return;
            const auto found = s.byTopic.find(message->topic);
            if (found == s.byTopic.end()) return;
            if (message->payloadlen == 0 || !message->payload) return;
            const std::string bytes(static_cast<const char*>(message->payload), static_cast<std::size_t>(message->payloadlen));
            for (const auto* binding : found->second) {
                try {
                    const auto properties = decodeMqttObservationPayload(*binding, bytes);
                    (void)s.owner.gate.accept(binding->targetId, observationEpoch, [&] {
                        s.owner.state.applyObservation({binding->targetId, properties, 0, std::nullopt});
                    });
                } catch (const std::exception&) {
                    // Duplicate/malformed/old samples fail closed; no partial update.
                }
            }
        }
        bool start() {
            try {
                configure(client, connection);
                mosquitto_connect_callback_set(client.ptr, onConnect);
                mosquitto_disconnect_callback_set(client.ptr, onDisconnect);
                mosquitto_message_callback_set(client.ptr, onMessage);
                // Subscription-only client may reconnect; it never publishes commands.
                (void)mosquitto_reconnect_delay_set(client.ptr, 1, 30, false);
                if (mosquitto_connect_async(client.ptr, connection.network.host.c_str(),
                        static_cast<int>(connection.network.port), 30) != MOSQ_ERR_SUCCESS ||
                    mosquitto_loop_start(client.ptr) != MOSQ_ERR_SUCCESS) return false;
                client.running = true;
                return true;
            } catch (const std::exception&) { return false; }
        }
    };
    // Declare after state reference: subscriptions stop and join before state dies.
    std::vector<std::unique_ptr<Subscription>> subscriptions;
#endif
    Impl(config::Catalog c, DeviceStateStore& s) : catalog(std::move(c)), state(s) {
#ifndef FIREFLY_WITH_MOSQUITTO
        for (const auto& connection : catalog->configuration().connections)
            if (connection.type == config::ExecutorKind::Mqtt && connection.enabled)
                throw config::ConfigError("mqtt_unavailable", "/connections",
                    "MQTT support is not compiled; enable FIREFLY_ENABLE_MQTT with libmosquitto before Apply");
#endif
    }
    HardwareResult execute(const config::Binding& binding) {
#ifdef FIREFLY_WITH_MOSQUITTO
        const auto* connection = config::findConnection(catalog->configuration(), binding.connectionId);
        if (!connection || !connection->enabled || connection->type != config::ExecutorKind::Mqtt)
            return error("mqtt_connection_missing");
        return publish(*connection, binding.mqtt);
#else
        (void)binding;
        return error("mqtt_unavailable");
#endif
    }
    void start() {
#ifdef FIREFLY_WITH_MOSQUITTO
        std::map<std::string, std::unique_ptr<Subscription>> sessions;
        for (const auto& o : catalog->configuration().observations) {
            if (o.executor != config::ExecutorKind::Mqtt) continue;
            const auto* connection = config::findConnection(catalog->configuration(), o.connectionId);
            if (!connection || !connection->enabled) continue;
            auto& session = sessions[o.connectionId];
            if (!session) session = std::make_unique<Subscription>(*this, *connection);
            session->byTopic[o.mqtt.topic].push_back(&o);
        }
        for (auto& [id, session] : sessions) {
            (void)id;
            if (!session->start()) session->disconnected();
            subscriptions.push_back(std::move(session));
        }
#endif
    }
};

MqttHardwareExecutor::MqttHardwareExecutor(config::Catalog catalog, DeviceStateStore& state)
    : impl_(std::make_unique<Impl>(std::move(catalog), state)) {}
MqttHardwareExecutor::~MqttHardwareExecutor() = default;

HardwareResult MqttHardwareExecutor::execute(const config::Binding& binding,
                                             const actions::GenericAction& action) {
    if (binding.executor != config::ExecutorKind::Mqtt ||
        binding.targetId != action.targetId() || binding.actionId != action.actionId())
        return error("binding_mismatch");
    return impl_->execute(binding);
}
HardwareObservation MqttHardwareExecutor::observe(const config::ObservationBinding&) {
    // MQTT subscriptions are asynchronous; the device state endpoint shows
    // confirmed samples. This is not an HTTP-like synchronous GET.
    return {error("mqtt_async_observation"), {}};
}
void MqttHardwareExecutor::beginCommand(const std::string& target) { impl_->gate.begin(target); }
void MqttHardwareExecutor::endCommand(const std::string& target) { impl_->gate.end(target); }
void MqttHardwareExecutor::startSubscriptions() {
    // Apply has already atomically published this generation. A broker client
    // allocation/runtime failure must not make a committed Apply appear failed.
    // Subscription delivery is best-effort; unknown state is the safe fallback.
    try { impl_->start(); }
    catch (const std::exception&) {
        for (const auto& observation : impl_->catalog->configuration().observations)
            if (observation.executor == config::ExecutorKind::Mqtt)
                impl_->state.invalidate(observation.targetId);
    }
}
}
