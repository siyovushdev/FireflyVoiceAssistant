#include "firefly/execution/MqttHardwareExecutor.h"
#include "firefly/execution/DeviceStateStore.h"
#include "firefly/config/Connectivity.h"
#include "firefly/actions/ScalarJson.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
std::uint64_t MqttObservationGate::snapshot(const std::string& target) const {
    (void)target;
    // Capture arrival order before competing with the publication mutex.
    // A callback already running cannot acquire a fresh post-publish ticket.
    return nextTicket_.fetch_add(1, std::memory_order_seq_cst);
}
void MqttObservationGate::begin(const std::string& target,
                                const std::map<std::string, actions::Scalar>& expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& current = targets_[target];
    if (current.inFlight) throw std::logic_error("mqtt_target_busy");
    // Copy can fail; don't leave the target stuck in-flight on allocation failure.
    current.expected = expected;
    current.cutoffTicket = nextTicket_.fetch_add(1, std::memory_order_seq_cst);
    current.inFlight = true;
    current.published = false;
    current.disconnectedDuringCommand = false;
    current.buffered.reset();
}
int MqttObservationGate::submitPublication(const std::string& target,
                                          const std::function<int()>& publish) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(target);
    if (found == targets_.end() || !found->second.inFlight || found->second.published)
        throw std::logic_error("mqtt_publish_outside_command");
    // A callback already entered before the publish call carries an older
    // lock-free arrival ticket, even if it must wait for this mutex. Record the
    // barrier immediately BEFORE invoking the MQTT API: a legitimate reply may
    // arrive from another thread while publish() is still returning.
    //
    // accept() cannot inspect or buffer replies while this mutex is held.
    // Only after a successful API return do we open the eligible reply window;
    // a failed/throwing publish can never turn those early tickets into state.
    found->second.cutoffTicket = nextTicket_.fetch_add(1, std::memory_order_seq_cst);
    const int result = publish();
    if (result == 0) { // MOSQ_ERR_SUCCESS
        found->second.published = true;
    }
    return result;
}
void MqttObservationGate::disconnected(const std::string& target, DeviceStateStore& store) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& current = targets_[target];
        current.cutoffTicket = nextTicket_.fetch_add(1, std::memory_order_seq_cst);
        if (current.inFlight) current.disconnectedDuringCommand = true;
        current.published = false;
        current.buffered.reset();
        store.invalidate(target);
        store.markUnreachable(target, store.nowMs());
    } catch (const std::exception&) {
        // Even OOM while acquiring gate storage cannot throw through a C callback.
    }
}
void MqttObservationGate::end(const std::string& target, DeviceStateStore& store,
                              bool accepted) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(target);
    if (found == targets_.end() || !found->second.inFlight) return;
    auto& current = found->second;
    // BoundDeviceExecutor has already written Predicted. A validated early
    // reply may now supersede it, but never after disconnect/failed publish.
    if (accepted && current.published && current.buffered) {
        try { store.applyObservation({target, *current.buffered, 0, std::nullopt}); }
        catch (const std::exception&) { /* leave Predicted, never false Confirmed */ }
    }
    current.buffered.reset();
    current.expected.clear();
    current.inFlight = false;
    current.published = false;
    if (current.disconnectedDuringCommand) {
        // BoundDeviceExecutor may have marked the publishing client reachable
        // after the subscription was lost. Disconnect wins the final state
        // even when publish and disconnect raced in the same millisecond.
        try {
            store.invalidate(target);
            store.markUnreachable(target, store.nowMs());
        } catch (const std::exception&) { /* fail closed */ }
    }
    // Keep tickets only from a healthy successful submission. An observation
    // captured during disconnect or a failed attempt cannot confirm later.
    if (!accepted || current.disconnectedDuringCommand)
        current.cutoffTicket = nextTicket_.fetch_add(1, std::memory_order_seq_cst);
    current.disconnectedDuringCommand = false;
}
bool MqttObservationGate::accept(const std::string& target, std::uint64_t version,
                                 const std::map<std::string, actions::Scalar>& properties,
                                 DeviceStateStore& store) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& current = targets_[target];
    // Callback execution may be reordered after arrival-ticket allocation.
    // Never let an earlier valid sample overwrite a later one. This fence is
    // per-target: commands/telemetry on other targets must not affect it.
    if (version <= current.cutoffTicket || version <= current.lastValidTicket) return false;
    const StateObservation sample{target, properties, 0, std::nullopt};
    if (current.inFlight) {
        if (!current.published || current.expected.empty()) return false;
        // Validate *before* replacing last known-good buffer. A malformed
        // sample (e.g. boolean power:7) must not evict a valid early reply.
        store.validateObservation(sample);
        // Generic IoT MQTT has no request correlation ID. Only an early sample
        // agreeing with an actual assigned property can confirm in-flight work.
        bool overlaps = false;
        bool contradicts = false;
        for (const auto& [key, expected] : current.expected) {
            const auto found = properties.find(key);
            if (found == properties.end()) continue;
            overlaps = true;
            if (found->second != expected) contradicts = true;
        }
        if (!overlaps) return false;
        if (contradicts) {
            // A newer well-typed OFF is still an authoritative arrival even
            // though it cannot confirm the requested ON. Revoke buffered ON
            // and prevent an older callback from recreating it.
            current.buffered.reset();
            current.lastValidTicket = version;
            return false;
        }
        // Build the replacement first; allocation/validation failures must
        // not advance the ticket fence or evict a valid buffered reply.
        auto next = properties;
        current.buffered = std::move(next);
        current.lastValidTicket = version;
        return true;
    }
    // Advance only after state validation/mutation succeeded. An invalid
    // late observation must not fence out earlier valid telemetry.
    store.applyObservation(sample);
    current.lastValidTicket = version;
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
// One publishing client per command. Bound network threads and avoid a
// reconnect queue growing without limit under concurrent hardware requests.
std::atomic<unsigned> activePublications{0};
struct PublicationSlot {
    bool acquired = false;
    PublicationSlot() noexcept {
        auto count = activePublications.load(std::memory_order_relaxed);
        while (count < 16 && !activePublications.compare_exchange_weak(
                count, count + 1, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {}
        acquired = count < 16;
    }
    ~PublicationSlot() {
        if (acquired) activePublications.fetch_sub(1, std::memory_order_release);
    }
};
struct Publication {
    std::mutex mutex;
    std::condition_variable cv;
    bool connected = false, connectionFinished = false, disconnected = false;
    int acknowledgedMid = -1;
};
void connected(mosquitto* mosq, void* context, int code) {
    auto& p = *static_cast<Publication*>(context);
    bool rejectReconnect = false;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        // This client is one-shot. An unexpected reconnect must never replay
        // an unresolved QoS 1 command, or make a failed send appear healthy.
        rejectReconnect = p.connectionFinished || p.disconnected;
        if (!rejectReconnect) {
            p.connected = code == 0;
            p.connectionFinished = true;
        }
    }
    if (rejectReconnect) (void)mosquitto_disconnect(mosq);
    p.cv.notify_all();
}
void published(mosquitto*, void* context, int id) {
    auto& p = *static_cast<Publication*>(context);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (!p.disconnected) p.acknowledgedMid = id; // can precede publish() return
    }
    p.cv.notify_all();
}
void disconnected(mosquitto* mosq, void* context, int code) {
    auto& p = *static_cast<Publication*>(context);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.connected = false;
        p.disconnected = true;
        p.connectionFinished = true;
    }
    p.cv.notify_all();
    // This client is one-shot: do not allow the background network loop to
    // reconnect and replay an unacknowledged QoS 1 publication.
    if (code != 0) (void)mosquitto_disconnect(mosq);
}
HardwareResult publish(const config::Connection& conn, const config::MqttBinding& mapping,
                       MqttObservationGate& gate, const std::string& target) {
    PublicationSlot slot;
    if (!slot.acquired) return error("mqtt_publish_busy");
    Publication p;
    try {
        Client client(&p);
        configure(client, conn);
        mosquitto_connect_callback_set(client.ptr, connected);
        mosquitto_publish_callback_set(client.ptr, published);
        mosquitto_disconnect_callback_set(client.ptr, disconnected);
        // libmosquitto documents connect_async together with loop_start.
        // Unlike subscription clients, this client publishes at most once.
        if (mosquitto_connect_async(client.ptr, conn.network.host.c_str(),
                static_cast<int>(conn.network.port), 30) != MOSQ_ERR_SUCCESS)
            return error("mqtt_connection_failure", ReachabilityEvidence::Unreachable);
        if (mosquitto_loop_start(client.ptr) != MOSQ_ERR_SUCCESS)
            return error("mqtt_connection_failure", ReachabilityEvidence::Unreachable);
        client.running = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(conn.timeoutMs);
        std::unique_lock<std::mutex> lock(p.mutex);
        if (!p.cv.wait_until(lock, deadline, [&] { return p.connectionFinished; }))
            return error("mqtt_timeout", ReachabilityEvidence::Unreachable);
        if (!p.connected || p.disconnected)
            return error("mqtt_connect_rejected", ReachabilityEvidence::Unreachable);
        lock.unlock();
        int mid = -1;
        // Serialize MQTT submit and subscription snapshots. A stale callback
        // cannot enter the post-submit buffer before this API call is made.
        const auto result = gate.submitPublication(target, [&] {
            return mosquitto_publish(client.ptr, &mid, mapping.topic.c_str(),
                static_cast<int>(mapping.payload.size()), mapping.payload.data(),
                static_cast<int>(mapping.qos), mapping.retain);
        });
        if (result != MOSQ_ERR_SUCCESS)
            return error("mqtt_publish_rejected", ReachabilityEvidence::Reachable);
        lock.lock();
        if (!p.cv.wait_until(lock, deadline, [&] { return p.acknowledgedMid == mid || p.disconnected; }))
            return error("mqtt_publish_uncertain", ReachabilityEvidence::Reachable);
        if (p.disconnected || p.acknowledgedMid != mid)
            return error("mqtt_publish_uncertain", ReachabilityEvidence::Reachable);
        // Delivery to the MQTT broker is not confirmation of device execution.
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
    // One subscription set per active RuntimeGeneration; a repeated startup
    // call must not create duplicate background loops for the same broker.
    std::once_flag subscriptionStartOnce;
#ifdef FIREFLY_WITH_MOSQUITTO
    struct Subscription {
        Impl& owner;
        const config::Connection& connection;
        Client client;
        std::map<std::string, std::vector<const config::ObservationBinding*>> byTopic;
        // Accessed only on this subscription's libmosquitto network loop.
        // No observation is authoritative until SUBACK accepts its topic.
        std::map<int, std::string> pendingSubacks;
        std::set<std::string> readyTopics;
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

        void invalidateTopic(const std::string& topic) noexcept {
            const auto it = byTopic.find(topic);
            if (it == byTopic.end()) return;
            for (const auto* binding : it->second)
                owner.gate.disconnected(binding->targetId, owner.state);
        }
        static void onConnect(mosquitto* client, void* ptr, int code) noexcept {
            auto& s = *static_cast<Subscription*>(ptr);
            try {
                s.readyTopics.clear();
                s.pendingSubacks.clear();
                if (code != 0) { s.disconnected(); return; }
                for (const auto& [topic, bindings] : s.byTopic) {
                    int qos = 0;
                    for (const auto* binding : bindings)
                        qos = std::max(qos, static_cast<int>(binding->mqtt.qos));
                    int mid = -1;
                    if (mosquitto_subscribe(client, &mid, topic.c_str(), qos) == MOSQ_ERR_SUCCESS)
                        s.pendingSubacks.emplace(mid, topic);
                    else s.invalidateTopic(topic);
                }
            } catch (const std::exception&) { s.disconnected(); }
        }
        static void onSubscribe(mosquitto*, void* ptr, int mid,
                                int grantedCount, const int* granted) noexcept {
            auto& s = *static_cast<Subscription*>(ptr);
            try {
                const auto found = s.pendingSubacks.find(mid);
                if (found == s.pendingSubacks.end()) return;
                auto topic = std::move(found->second);
                s.pendingSubacks.erase(found);
                // MQTT v3.1.1 reports rejected SUBSCRIBE using granted QoS 0x80.
                if (grantedCount != 1 || !granted || granted[0] < 0 || granted[0] > 1) {
                    s.invalidateTopic(topic);
                    return;
                }
                s.readyTopics.insert(std::move(topic));
            } catch (const std::exception&) { s.disconnected(); }
        }
        static void onDisconnect(mosquitto*, void* ptr, int) noexcept {
            static_cast<Subscription*>(ptr)->disconnected();
        }
        void disconnected() noexcept {
            readyTopics.clear();
            pendingSubacks.clear();
            for (const auto& [topic, bindings] : byTopic) {
                (void)topic;
                for (const auto* binding : bindings)
                    owner.gate.disconnected(binding->targetId, owner.state);
            }
        }
        static void onMessage(mosquitto*, void* ptr, const mosquitto_message* message) noexcept {
            try {
                auto& s = *static_cast<Subscription*>(ptr);
                // Capture the callback-entry ticket before topic lookup, vector
                // allocation or payload parsing. One globally ordered arrival
                // ticket can be reused for all targets in this MQTT message;
                // each target still has its own cutoff/command state.
                const auto arrivalTicket = s.owner.gate.snapshot({});
                if (!message || !message->topic || message->retain ||
                    message->payloadlen <= 0 || message->payloadlen > 4096 || !message->payload) return;
                const auto found = s.byTopic.find(message->topic);
                if (found == s.byTopic.end() || !s.readyTopics.count(found->first)) return;
                // Attach the callback-entry ticket to every affected target
                // before copying/parsing a potentially slow message.
                std::vector<std::pair<const config::ObservationBinding*, std::uint64_t>> observed;
                observed.reserve(found->second.size());
                for (const auto* binding : found->second)
                    observed.emplace_back(binding, arrivalTicket);
                const std::string bytes(static_cast<const char*>(message->payload),
                                        static_cast<std::size_t>(message->payloadlen));
                for (const auto& [binding, version] : observed) {
                    try {
                        const auto properties = decodeMqttObservationPayload(*binding, bytes);
                        (void)s.owner.gate.accept(binding->targetId, version,
                                                   properties, s.owner.state);
                    } catch (const std::exception&) {
                        // Malformed/obsolete samples fail closed, without partial state.
                    }
                }
            } catch (const std::exception&) {
                // No exception, including allocation failure, may cross a C callback.
            }
        }
        bool start() {
            try {
                configure(client, connection);
                mosquitto_connect_callback_set(client.ptr, onConnect);
                mosquitto_disconnect_callback_set(client.ptr, onDisconnect);
                mosquitto_subscribe_callback_set(client.ptr, onSubscribe);
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
        return publish(*connection, binding.mqtt, gate, binding.targetId);
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
    : impl_(std::make_unique<Impl>(std::move(catalog), state)) {
#ifdef FIREFLY_WITH_MOSQUITTO
    // Initialise the library once, before any subscription or publisher loop
    // is started. This is process-local library setup, not a broker connection:
    // constructing/preflighting a draft still performs no MQTT network I/O.
    initializeLibrary();
#endif
}
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
void MqttHardwareExecutor::beginCommand(
    const std::string& target, const std::map<std::string, actions::Scalar>& expected) {
    impl_->gate.begin(target, expected);
}
void MqttHardwareExecutor::endCommand(const std::string& target, bool accepted) {
    impl_->gate.end(target, impl_->state, accepted);
}
void MqttHardwareExecutor::startSubscriptions() {
    // Apply has already atomically published this generation. A broker client
    // allocation/runtime failure must not make a committed Apply appear failed.
    // Subscription delivery is best-effort; unknown state is the safe fallback.
    std::call_once(impl_->subscriptionStartOnce, [this] {
        try { impl_->start(); }
        catch (const std::exception&) {
            for (const auto& observation : impl_->catalog->configuration().observations)
                if (observation.executor == config::ExecutorKind::Mqtt)
                    impl_->gate.disconnected(observation.targetId, impl_->state);
        }
    });
}
}
