#pragma once
#include "firefly/ParserRoomDomain.h"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace firefly {
class RoomLookupMismatch final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class DeterministicParser {
public:
    // Production requires explicit immutable room data; rules are compiled once.
    explicit DeterministicParser(const ParserRoomDomain& domain);
    // Production: compare only, throw on disagreement, never use the old answer.
    std::optional<std::string> parseChecked(std::string_view original) const;
    void requireLegacyRoomDomain() const;
    // Pure configured candidate, also used to test deliberate configuration drift.
    // This does not authorize execution; production must use parseChecked.
    std::optional<std::string> parseConfigured(std::string_view original) const;
    // Migration-only legacy oracle for frozen tests/reference harnesses.
    // Production never uses this overload as a fallback.
    static std::optional<std::string> parse(std::string_view original);
private:
    struct Rules;
    const std::shared_ptr<const Rules> rules_;
};
}
