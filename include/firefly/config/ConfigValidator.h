#pragma once
#include "firefly/config/Configuration.h"

namespace firefly::config {
class ConfigValidator final {
public:
    static void validateIdentityName(const Identity& value, const std::string& path);
    // Revalidates all fields, including manually constructed values.
    static Catalog validate(Configuration configuration);
};
}
