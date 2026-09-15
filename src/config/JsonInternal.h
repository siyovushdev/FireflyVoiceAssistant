#pragma once
#include "firefly/config/Configuration.h"
#include <nlohmann/json.hpp>

namespace firefly::config::detail {
using Json = nlohmann::json;
Json parseDocument(std::string_view bytes);
void validateStructure(const Json& value);
Configuration decode(const Json& value);
Json encode(const Configuration& value);
std::string operationName(Operation value);
std::string kindName(DeviceKind value);
}
