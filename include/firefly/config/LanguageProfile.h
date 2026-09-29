#pragma once
#include "firefly/config/Configuration.h"
namespace firefly::config {
struct LanguageConflict {
    std::string severity, code, alias;
    std::vector<std::string> entities;
};
std::string normalizeLanguageAtom(std::string_view value);
void validateLanguageAtom(std::string_view value, bool action = false);
std::vector<LanguageConflict> languageConflicts(const Configuration& configuration);
}
