#pragma once
#include "firefly/ParserRoomDomain.h"
#include "firefly/actions/GenericAction.h"
#include <memory>
#include <string_view>
namespace firefly {
class DeterministicParser {
public:
    explicit DeterministicParser(const ParserRoomDomain& domain);
    // Unsupported here means unresolved grammar; the engine may call the LLM.
    actions::CandidateResult parse(std::string_view original) const;
private:
    struct Rules;
    const std::shared_ptr<const Rules> rules_;
};
}
