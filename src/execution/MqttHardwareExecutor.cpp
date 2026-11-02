#include "firefly/execution/MqttHardwareExecutor.h"
#include "firefly/execution/MqttCredentials.h"
#include "MqttLibrary.h"
#include "MqttProcess.h"
#include "MqttPublishProcess.h"
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
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef FIREFLY_WITH_MOSQUITTO
#include <mosquitto.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace firefly::execution {
MqttObservationGate::MqttObservationGate() : sharedOrder_(std::make_unique<detail::MqttSharedOrder>()) {}
MqttObservationGate::~MqttObservationGate() = default;
int MqttObservationGate::publicationOrderDescriptor() const { return sharedOrder_->descriptor(); }
std::uint64_t MqttObservationGate::nextTicket() const {
    return sharedOrder_->valid() ? sharedOrder_->next() : nextTicket_.fetch_add(1, std::memory_order_seq_cst);
}
std::uint64_t MqttObservationGate::snapshot(const std::string& target) const {
    (void)target;
    // Capture arrival order before competing with the publication mutex.
    // A callback already running cannot acquire a fresh post-publish ticket.
    return nextTicket();
}
void MqttObservationGate::begin(const std::string& target,
                                const std::map<std::string, actions::Scalar>& expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& current = targets_[target];
    if (current.inFlight) throw std::logic_error("mqtt_target_busy");
    // Copy can fail; don't leave the target stuck in-flight on allocation failure.
    current.expected = expected;
    current.cutoffTicket = nextTicket();
    current.inFlight = true;
    current.published = false;
    current.pendingSubmission = false;
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
    found->second.cutoffTicket = nextTicket();
    const int result = publish();
    if (result == 0) { // MOSQ_ERR_SUCCESS
        found->second.published = true;
    }
    return result;
}
std::uint64_t MqttObservationGate::authorizePublication(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& current = targets_.at(target);
    if (!current.inFlight || current.published) throw std::logic_error("mqtt_publish_outside_command");
    current.cutoffTicket = nextTicket();
    current.published = true;
    current.pendingSubmission = true;
    return current.cutoffTicket;
}
void MqttObservationGate::publicationSubmitted(const std::string& target, std::uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& current = targets_.at(target);
    if (!current.inFlight || !current.pendingSubmission) throw std::logic_error("mqtt_publish_outside_command");
    current.cutoffTicket = std::max(current.cutoffTicket, ticket);
    current.pendingSubmission = false;
    if (current.buffered && current.bufferedTicket <= current.cutoffTicket) current.buffered.reset();
}
void MqttObservationGate::disconnected(const std::string& target, DeviceStateStore& store) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& current = targets_[target];
        current.cutoffTicket = nextTicket();
        if (current.inFlight) current.disconnectedDuringCommand = true;
        current.published = false;
        current.buffered.reset();
        store.observationDisconnected(target);
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
    accepted = accepted && !current.pendingSubmission;
    // BoundDeviceExecutor has already written Predicted. A validated early
    // reply may now supersede it, but never after disconnect/failed publish.
    if (accepted && current.published && !current.pendingSubmission && current.buffered &&
        current.bufferedTicket > current.cutoffTicket) {
        try { store.applyObservation({target, *current.buffered, 0, std::nullopt}); }
        catch (const std::exception&) { /* leave Predicted, never false Confirmed */ }
    }
    current.buffered.reset();
    current.expected.clear();
    current.inFlight = false;
    current.published = false;
    current.pendingSubmission = false;
    if (current.disconnectedDuringCommand) {
        // BoundDeviceExecutor may have marked the publishing client reachable
        // after the subscription was lost. Disconnect wins the final state
        // even when publish and disconnect raced in the same millisecond.
        try {
            store.observationDisconnected(target);
        } catch (const std::exception&) { /* fail closed */ }
    }
    // Keep tickets only from a healthy successful submission. An observation
    // captured during disconnect or a failed attempt cannot confirm later.
    if (!accepted || current.disconnectedDuringCommand)
        current.cutoffTicket = nextTicket();
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
        current.bufferedTicket = version;
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
    // Validate the *entire* declared flat state contract before projecting any
    // properties. Previously an extra JSON key was silently ignored, letting
    // {"power":true,"unknown":...} establish false Confirmed knowledge.
    // Unknown keys (including nested objects and arrays) must reject the
    // complete observation, not merely disappear from the mapped result.
    for (auto it = json.begin(); it != json.end(); ++it)
        if (binding.fields.find(it.key()) == binding.fields.end())
            throw std::runtime_error("mqtt_unknown_json_state_field");
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
    detail::ensureMqttLibrary();
}
// Admin and runtime use the same strict on-disk reader. Never trust a
// credentials_id as a path or bypass a private-directory permission check.
MqttLogin readLogin(const std::string& id) {
    return MqttCredentials{}.read(id);
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
        if (mosquitto_tls_set(client.ptr, nullptr, "/etc/ssl/certs", nullptr, nullptr, nullptr) != MOSQ_ERR_SUCCESS)
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
HardwareResult publish(const config::Connection& conn, const config::MqttBinding& mapping,
                       MqttObservationGate& gate, const std::string& target) {
    PublicationSlot slot;
    if (!slot.acquired) return error("mqtt_publish_busy");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(conn.timeoutMs);
    return detail::supervisedMqttPublish(conn, mapping, gate, target, deadline);
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
        // If initial connect_async fails synchronously (e.g. ECONNREFUSED),
        // there is no libmosquitto network loop to reconnect automatically.
        // One bounded retry worker exists only while that broker is offline.
        std::mutex retryMutex;
        std::condition_variable retryWake;
        bool stopping = false;
        std::thread initialRetry;
        Subscription(Impl& owner, const config::Connection& c)
            : owner(owner), connection(c), client(this) {}
        ~Subscription() {
            // Stop and join our initial-connect retry before accessing/destroying
            // the libmosquitto handle. A successful retry may have started its
            // network loop, so it must complete before the regular loop join.
            {
                std::lock_guard<std::mutex> lock(retryMutex);
                stopping = true;
            }
            retryWake.notify_all();
            if (initialRetry.joinable()) initialRetry.join();
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
        bool tryStartNetworkLoop() noexcept {
            try {
                // Only one thread calls this until the first successful loop
                // startup. Once started, libmosquitto owns normal reconnects.
                if (mosquitto_connect_async(client.ptr, connection.network.host.c_str(),
                        static_cast<int>(connection.network.port), 30) != MOSQ_ERR_SUCCESS)
                    return false;
                if (mosquitto_loop_start(client.ptr) != MOSQ_ERR_SUCCESS)
                    return false;
                client.running = true;
                return true;
            } catch (const std::exception&) { return false; }
        }
        void retryInitialConnect() noexcept {
            std::unique_lock<std::mutex> lock(retryMutex);
            auto delay = std::chrono::seconds(1);
            while (!stopping) {
                if (retryWake.wait_for(lock, delay, [this] { return stopping; }))
                    break;
                lock.unlock();
                const bool started = tryStartNetworkLoop();
                lock.lock();
                if (started) break;
                delay = std::min(delay * 2, std::chrono::seconds(30));
            }
        }
        bool start() {
            try {
                configure(client, connection);
                mosquitto_connect_callback_set(client.ptr, onConnect);
                mosquitto_disconnect_callback_set(client.ptr, onDisconnect);
                mosquitto_subscribe_callback_set(client.ptr, onSubscribe);
                mosquitto_message_callback_set(client.ptr, onMessage);
                // Subscribe-only clients never publish/replay commands.
                (void)mosquitto_reconnect_delay_set(client.ptr, 1, 30, false);
                if (tryStartNetworkLoop()) return true;

                // Synchronous initial refusal means no running loop. The
                // subscription remains Unknown/Unreachable while retrying,
                // and fresh telemetry after recovery is validated as usual.
                disconnected();
                initialRetry = std::thread([this] { retryInitialConnect(); });
                return true; // retry owned by this generation, not a failed Apply
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
            // Finite, documented per-generation limits. Reject a dangerously
            // large subscription graph before starting any network loops.
            if (!session) {
                if (sessions.size() > 32)
                    throw std::runtime_error("mqtt_subscription_client_limit");
                session = std::make_unique<Subscription>(*this, *connection);
            }
            if (!session->byTopic.count(o.mqtt.topic) && session->byTopic.size() >= 128)
                throw std::runtime_error("mqtt_subscription_topic_limit");
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
    // Initialise once before any configured MQTT client threads start. Pure
    // HTTP runtimes must not depend on libmosquitto's initialization status.
    // This is local library setup, not broker contact during draft/preflight.
    for (const auto& connection : impl_->catalog->configuration().connections) {
        if (connection.type == config::ExecutorKind::Mqtt && connection.enabled) {
            initializeLibrary();
            break;
        }
    }
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
void MqttHardwareExecutor::stopSubscriptions() noexcept {
#ifdef FIREFLY_WITH_MOSQUITTO
    // Stop/join each subscription callback thread before releasing byTopic,
    // the associated gate and DeviceStateStore. No publishers are replayed or
    // cancelled here: one-shot command clients have separate ownership.
    impl_->subscriptions.clear();
#endif
}
}
