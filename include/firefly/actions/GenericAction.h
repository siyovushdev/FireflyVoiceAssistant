#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace firefly::actions {
// Schema v1 supports only integer arguments. Require an exact int64_t input:
// no implicit bool, floating-point, string or unsigned-to-signed coercion.
class IntegerArgument final {
public:
    explicit IntegerArgument(std::int64_t value) : value_(value) {}
    template<class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::int64_t> &&
                                     !std::is_same_v<std::decay_t<T>, IntegerArgument>, int> = 0>
    IntegerArgument(T&&) = delete;
    std::int64_t value() const { return value_; }
    bool operator==(const IntegerArgument& other) const { return value_ == other.value_; }
    bool operator!=(const IntegerArgument& other) const { return !(*this == other); }
private:
    std::int64_t value_;
};

// Semantic candidate value, NOT a validated/executable command. IDs name schema
// objects; no compact codes, display names, configuration ownership or safety
// decisions live here. Unknown IDs/argument sets can be represented for rejection
// at a validation/adapter boundary. Accessors expose no mutable aliases.
class GenericAction final {
public:
    using Arguments = std::map<std::string, IntegerArgument>;
    GenericAction(std::string actionId, std::string targetId, Arguments arguments);
    const std::string& actionId() const { return actionId_; }
    const std::string& targetId() const { return targetId_; }
    const Arguments& arguments() const { return arguments_; }
    bool operator==(const GenericAction& other) const;
    bool operator!=(const GenericAction& other) const { return !(*this == other); }
private:
    std::string actionId_;
    std::string targetId_;
    Arguments arguments_;
};

struct Unsupported final {};
inline bool operator==(Unsupported, Unsupported) { return true; }
inline bool operator!=(Unsupported, Unsupported) { return false; }
// Default construction is Unsupported, never an invented/default action.
using CandidateResult = std::variant<Unsupported, GenericAction>;
}
