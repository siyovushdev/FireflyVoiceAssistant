#pragma once
#include "firefly/config/Configuration.h"
namespace firefly::config {
struct LanguageConflict {
    std::string severity, code, alias;
    std::vector<std::string> entities;
};
std::string normalizeLanguageAtom(std::string_view value);
void validateLanguageAtom(std::string_view value, bool action = false);
// Bounded grammatical forms derived from approved location atoms, never from
// user command text. Returned phrases remain evidence for this exact location.
std::vector<std::string> locationGrammar(const Room& room);
std::vector<LanguageConflict> languageConflicts(const Configuration& configuration);
}
