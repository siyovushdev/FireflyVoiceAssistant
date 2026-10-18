#include "MqttProcess.h"
#include "MqttPublishProcess.h"
#include "firefly/execution/MqttCredentials.h"
#include <set>
extern char** environ;

namespace firefly::execution::detail {
HardwareResult supervisedMqttPublish(const config::Connection& connection,
    const config::MqttBinding& mapping, MqttObservationGate& gate,
    const std::string& target, MqttClock::time_point deadline) {
    bool connected = false, permitted = false;
    const auto failed = [&](const std::string& code) -> HardwareResult {
        return {false, code, {}, connected ? ReachabilityEvidence::Reachable : ReachabilityEvidence::Unreachable};
    };
    const auto interrupted = [&] {
        return failed(permitted ? "mqtt_publish_uncertain" :
                      mqttRemaining(deadline) ? "mqtt_publish_worker_unavailable" : "mqtt_timeout");
    };
    try {
        // Credentials are read within the same budget, only by the parent.
        const auto login = MqttCredentials{}.read(connection.mqtt.credentialsId);
        if (!mqttRemaining(deadline)) return failed("mqtt_timeout");
        const auto worker = mqttPublishWorkerPath();
        const int order = gate.publicationOrderDescriptor();
        if (order < 0) return failed("mqtt_publish_worker_unavailable");
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
            return failed("mqtt_publish_worker_unavailable");
        MqttFd parent(sockets[0]), source(sockets[1]);
        MqttFd socketCopy(::fcntl(source.value, F_DUPFD_CLOEXEC, 4));
        MqttFd orderCopy(::fcntl(order, F_DUPFD_CLOEXEC, 4));
        if (socketCopy.value < 0 || orderCopy.value < 0) return failed("mqtt_publish_worker_unavailable");
        struct Actions {
            posix_spawn_file_actions_t value{};
            bool valid = posix_spawn_file_actions_init(&value) == 0;
            ~Actions() { if (valid) posix_spawn_file_actions_destroy(&value); }
        } actions;
        if (!actions.valid ||
            posix_spawn_file_actions_adddup2(&actions.value, socketCopy.value, 0) ||
            posix_spawn_file_actions_adddup2(&actions.value, orderCopy.value, 3) ||
            posix_spawn_file_actions_addopen(&actions.value, 1, "/dev/null", O_WRONLY, 0600) ||
            posix_spawn_file_actions_addopen(&actions.value, 2, "/dev/null", O_WRONLY, 0600) ||
            posix_spawn_file_actions_addclosefrom_np(&actions.value, 4))
            return failed("mqtt_publish_worker_unavailable");
        char* argv[]{const_cast<char*>(worker.c_str()), nullptr};
        pid_t pid = -1;
        if (::posix_spawn(&pid, worker.c_str(), &actions.value, nullptr, argv, environ) || pid <= 0)
            return failed("mqtt_publish_worker_unavailable");
        MqttChild child(pid);
        // Close every parent copy of the child's socket end. EOF then really
        // represents worker exit; no descriptor can keep cancellation alive.
        ::close(source.value); source.value = -1;
        ::close(socketCopy.value); socketCopy.value = -1;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
        const nlohmann::json request{{"host",connection.network.host},{"port",connection.network.port},
            {"tls",connection.mqtt.tls},{"username",login.user},{"password",login.password},
            {"deadline_ns",ns},{"parent_pid",::getpid()},{"topic",mapping.topic},{"payload",mapping.payload},
            {"qos",mapping.qos},{"retain",mapping.retain}};
        nlohmann::json reply;
        if (!mqttSend(parent.value, request, deadline) || !mqttReceive(parent.value, reply, deadline))
            return interrupted();
        if (reply != nlohmann::json{{"stage","connected"}}) {
            if (reply.size() != 2 || reply.value("stage","") != "failed") return interrupted();
            const auto code = reply.at("code").get<std::string>();
            if (code != "mqtt_timeout" && code != "mqtt_connect_rejected" &&
                code != "mqtt_connection_failure" && code != "mqtt_configuration_or_credentials_error")
                return interrupted();
            if (!child.reap(deadline) || !mqttRemaining(deadline)) return interrupted();
            return failed(code);
        }
        connected = true;
        if (!mqttRemaining(deadline)) return failed("mqtt_timeout");
        const auto authorizedTicket = gate.authorizePublication(target);
        // Waiting for a child must NEVER hold the generation's observation
        // mutex: another target has its own absolute deadline and may publish.
        permitted = true;
        if (!mqttSend(parent.value, {{"publish",true}}, deadline) ||
            !mqttReceive(parent.value, reply, deadline)) return interrupted();
        if (reply.size() != 2 || reply.value("stage","") != "submitted" ||
            !reply.at("ticket").is_number_unsigned()) return interrupted();
        const auto submitted = reply.at("ticket").get<std::uint64_t>();
        if (submitted <= authorizedTicket) return interrupted();
        gate.publicationSubmitted(target, submitted);
        if (!mqttReceive(parent.value, reply, deadline)) return interrupted();
        if (reply != nlohmann::json{{"stage","accepted"}}) {
            if (reply.size() != 2 || reply.value("stage","") != "failed" ||
                reply.value("code","") != "mqtt_publish_uncertain") return interrupted();
            return failed("mqtt_publish_uncertain");
        }
        if (!child.reap(deadline) || !mqttRemaining(deadline)) return interrupted();
        // A broker acknowledgement never establishes physical device state.
        return {true, {}, {}, ReachabilityEvidence::Reachable};
    } catch (const std::exception& error) {
        if (!permitted && std::string(error.what()) == "mqtt_publish_worker_unavailable")
            return failed("mqtt_publish_worker_unavailable");
        return failed(permitted ? "mqtt_publish_uncertain" : "mqtt_configuration_or_credentials_error");
    }
}
} // namespace firefly::execution::detail
