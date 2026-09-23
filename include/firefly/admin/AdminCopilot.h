#pragma once
#include <string>
#include <string_view>
#include "firefly/config/Configuration.h"

namespace firefly::admin {
// Local Admin Copilot: natural language → structured operations → deterministic
// application onto a COPY of the current Admin draft. Never Save/Apply.
std::string copilotPrompt(config::Catalog draft);
std::string decodeCopilotProposal(std::string_view raw, config::Catalog draft);
std::string acceptCopilotProposal(std::string_view currentDraftJson, std::string_view proposalJson);
std::string draftFingerprint(config::Catalog draft);
}
