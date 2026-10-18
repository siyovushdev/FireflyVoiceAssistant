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
    static MqttProbeResult testConnection(const config::Connection& connection,
                                          const MqttCredentials& credentials);
};
}
