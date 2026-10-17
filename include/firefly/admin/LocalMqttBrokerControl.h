#pragma once
#include <string>

namespace firefly::admin {
// Client-side view of the fixed, socket-activated privileged MQTT broker
// controller. Neither configuration edits nor preview call this interface.
struct LocalMqttBrokerStatus final {
    bool installed = false;
    bool enabled = false;
    bool running = false;
};
class LocalMqttBrokerControl final {
public:
    static constexpr const char* DefaultSocket = "/run/firefly-mqtt-control/control.sock";
    explicit LocalMqttBrokerControl(std::string socketPath = DefaultSocket,
                                    unsigned trustedPeerUid = 0);
    LocalMqttBrokerStatus status() const;
    LocalMqttBrokerStatus enable() const;
    LocalMqttBrokerStatus disable() const;
private:
    LocalMqttBrokerStatus request(const char* verb) const;
    std::string socketPath_;
    unsigned trustedPeerUid_ = 0;
};
}
