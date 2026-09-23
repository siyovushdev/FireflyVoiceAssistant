#include "firefly/config/Configuration.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace firefly::config {
bool accepts(const Parameter& schema, const actions::Scalar& value) {
    const auto& data = value.data();
    if (schema.type == "boolean") return std::holds_alternative<bool>(data);
    if (schema.type == "integer") {
        const auto* n = std::get_if<std::int64_t>(&data);
        // JSON/browser boundary deliberately accepts only exact safe integers.
        return n && *n >= -MaxRevision && *n <= MaxRevision && *n >= schema.minimum && *n <= schema.maximum;
    }
    if (schema.type == "number") {
        if (const auto* integer = std::get_if<std::int64_t>(&data))
            return *integer >= -MaxRevision && *integer <= MaxRevision && *integer >= schema.numberMinimum && *integer <= schema.numberMaximum;
        const auto* n = std::get_if<double>(&data);
        return n && std::isfinite(*n) && *n >= schema.numberMinimum && *n <= schema.numberMaximum;
    }
    const auto* text = std::get_if<std::string>(&data);
    if (!text) return false;
    // Programmatic callers must obey the same strict UTF-8 boundary as JSON ingress.
    try { (void)nlohmann::json(*text).dump(); } catch (const std::exception&) { return false; }
    if (schema.type == "enum") return std::find(schema.values.begin(), schema.values.end(), *text) != schema.values.end();
    if (schema.type != "string") return false;
    const auto length = std::count_if(text->begin(), text->end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    return static_cast<std::size_t>(length) >= schema.minLength && static_cast<std::size_t>(length) <= schema.maxLength &&
        std::none_of(text->begin(), text->end(), [](unsigned char c) { return c < 32 || c == 127; });
}
Configuration migrateV2(Configuration value) {
    if (value.schemaVersion == 2) return value;
    if (value.schemaVersion != 1) throw ConfigError("unsupported_version", "/schema_version", "Cannot migrate this version");
    value.schemaVersion = 2;
    // Legacy operations keep their compiled semantics and protected bindings.
    // Explicit state is only needed for new declarative mock operations.
    // Existing metadata and stable identities are copied without reinterpretation.
    return value;
}
}
