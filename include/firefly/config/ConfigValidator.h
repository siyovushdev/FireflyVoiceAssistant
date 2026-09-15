#pragma once
#include "firefly/config/Configuration.h"

namespace firefly::config {
class ConfigValidator final {
public:
    // Revalidates all fields, including manually constructed values.
    static Catalog validate(Configuration configuration);
};
}
