#pragma once

namespace firefly::execution::detail {
// Exactly one libmosquitto initialization shared by production MQTT
// publishing/subscriptions and short-lived Admin CONNECT probes.
void ensureMqttLibrary();
}
