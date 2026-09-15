#pragma once
#include "firefly/config/Configuration.h"
#include <string_view>

namespace firefly::config {
class ConfigLoader final {
public:
    static Catalog fromJson(std::string_view bytes);
    // Rejects symlink path components and non-regular files; bounded reads.
    static Catalog fromFile(const std::string& path);
};
}
