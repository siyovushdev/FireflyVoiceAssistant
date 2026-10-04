#pragma once
#include <string>
#include <string_view>
#include <set>
#include "firefly/config/Configuration.h"

namespace firefly::admin {
// Version 1: UTF-8 Russian transliteration, ASCII lowercase, collapsed separators,
// 64-byte bound, deterministic numeric collision suffix. Never called on rename.
std::string deterministicId(std::string_view category, std::string_view name,
                            const std::set<std::string>& occupied = {});
std::string suggestionPrompt(std::string_view entity);
std::string templatesJson();
// initialLocation is internal to first setup enrichment of a new location.
// It replaces only the generated "в + name" placeholder, never reviewed profiles.
std::string languageProfilePrompt(config::Catalog, std::string_view entity, std::string_view id, bool initialLocation = false);
std::string decodeLanguageProfile(std::string_view raw, config::Catalog, std::string_view entity, std::string_view id, bool initialLocation = false);
std::string draftPrompt(std::string_view entity, std::string_view group);
std::string decodeDraftGroup(std::string_view raw, std::string_view entity, std::string_view group);
std::string planPrompt();
std::string decodePlan(std::string_view raw, config::Catalog candidate);
std::string actionDraftPrompt(config::Catalog candidate, std::string_view targetId);
std::string decodeActionDraft(std::string_view raw, config::Catalog candidate, std::string_view targetId, std::string_view name);
// Strict, bounded JSON decoding. No extraction, markdown stripping or repair.
std::string decodeSuggestion(std::string_view raw, std::string_view entity = {});
std::string configurationDiff(std::string_view before, std::string_view after);
}
