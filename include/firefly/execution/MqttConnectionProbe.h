#pragma once
#include "firefly/config/Configuration.h"
#include "firefly/execution/MqttCredentials.h"
#include <string>

namespace firefly::execution {

// A disposable, independent read-only MQTT client. Success means only that
// the broker accepted one MQTT CONNECT with the supplied credentials and,
// if configured, a verified TLS handshake at that instant. It never mutates
// the runtime, subscribes, publishes, or starts a local broker.
struct MqttProbeResult final {
    bool success = false;
    std::string code;
    bool tlsVerified = false;
    bool credentialsAccepted = false;
};
class MqttConnectionProbe final {
public:
    // Parent-side supervisor. The entire wall-clock budget includes DNS,
    // MQTT CONNECT/CONNACK and termination/reaping of a disposable worker.
    static MqttProbeResult testConnection(const config::Connection& connection,
                                          const MqttCredentials& credentials);
};
namespace detail {
    // Only the dedicated fixed-path firefly-mqtt-probe-worker calls this.
    // It runs in a separate process so blocking libc DNS can be terminated.
    MqttProbeResult runMqttProbeWorker(const config::Connection& connection,
                                      const MqttLogin& login);
}
}
