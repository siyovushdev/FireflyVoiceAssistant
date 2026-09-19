#pragma once
#include "firefly/CommandEngine.h"
#include "firefly/SafetyGate.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/runtime/RuntimeConfig.h"
#include "firefly/legacy/ActionValidator.h"
#include "reference/FrozenSemantics.h"
#include <stdexcept>
namespace test {
// Test-only presentation helpers retain the frozen compact expected matrices.
// All decisions run through the real typed production components.
struct Fixture {
    firefly::config::Catalog snapshot = firefly::config::ConfigLoader::fromFile(std::string(FIREFLY_TEST_ROOT) + "/config/current-m4.json");
    firefly::runtime::DomainView domain{snapshot};
    firefly::DeterministicParser parser{domain};
    firefly::actions::CompactV1Adapter codec{snapshot};
};
inline const Fixture& fixture() { static const Fixture value; return value; }
inline std::string wire(const firefly::actions::CandidateResult& value) { return fixture().codec.render(value); }
inline firefly::actions::CandidateResult decode(std::string_view wire) {
    const auto result = fixture().codec.decode(wire);
    if (const auto* value = std::get_if<firefly::actions::CandidateResult>(&result)) return *value;
    return firefly::actions::Unsupported{};
}
inline std::optional<std::string> parse(std::string_view input) {
    const auto result = fixture().parser.parse(input);
    if (std::holds_alternative<firefly::actions::Unsupported>(result)) return std::nullopt;
    return wire(result);
}
inline std::string gate(std::string_view original, std::string_view compact) {
    const auto candidate = decode(compact);
    const auto result = firefly::SafetyGate::apply(original, candidate, fixture().domain);
    if (std::holds_alternative<firefly::actions::Unsupported>(result)) return "U";
    if (result != candidate) throw std::runtime_error("SafetyGate repaired a semantic candidate");
    // Preserve the original lexical form only for old preserve-or-reject matrix
    // assertions (e.g. leading zeros). Production never carries this wire form.
    return std::string(compact);
}
struct Result : firefly::CommandResult {
    explicit Result(firefly::CommandResult value) : firefly::CommandResult(std::move(value)) {}
    std::string compact() const { return wire(action); }
};
class Engine {
public:
    explicit Engine(firefly::LlmEngine& llm) : engine_(llm, fixture().parser, fixture().domain) {}
    Engine(firefly::LlmEngine& llm, const firefly::DeterministicParser& parser)
        : engine_(llm, parser, fixture().domain) {}
    Result process(std::string_view original) { return Result(engine_.process(original)); }
private:
    firefly::CommandEngine engine_;
};
}
