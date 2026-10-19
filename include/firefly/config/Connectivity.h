#pragma once
#include "firefly/config/Configuration.h"

namespace firefly::config {
std::string executorName(ExecutorKind kind);
ExecutorKind executorKind(const std::string& name);
const Connection* findConnection(const Configuration& config, const std::string& id);
// Validates only transport contracts; domain references remain ConfigValidator's job.
void validateConnections(const Configuration& config);
void validateMqttBinding(const MqttBinding& binding, const Connection* connection);
void validateMqttObservation(const MqttObservation& observation, const Connection* connection);
void validateHttpMapping(const HttpBinding& http, const Connection* connection, bool observe = false);
HttpBinding effectiveHttp(const HttpBinding& mapping, const Connection* connection);
}
