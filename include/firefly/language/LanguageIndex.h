#pragma once
#include "firefly/runtime/DomainView.h"
#include "firefly/config/LanguageProfile.h"
namespace firefly::language {
enum class MatchKind { ExactUnique, FuzzyUnique, Miss, Ambiguous, Invalid };
const char* matchName(MatchKind kind);
struct MatchResult {
    MatchKind kind = MatchKind::Miss;
    actions::CandidateResult candidate;
    std::string reason;
    std::string stage = "input", targetId, actionId;
    std::vector<std::string> uncovered;
};
class LanguageCompiler;
class LanguageIndex final {
public:
    MatchResult match(std::string_view original) const;
    const std::string& catalogHash() const { return domain_.snapshot()->contentHash(); }
    std::int64_t revision() const { return domain_.snapshot()->configuration().revision; }
    const std::string& fingerprint() const { return fingerprint_; }
    static constexpr const char* CompilerVersion = "language_atoms_v2_location_grammar";
    const std::vector<config::LanguageConflict>& conflicts() const { return conflicts_; }
    std::size_t approximateBytes() const { return bytes_; }
private:
    friend class LanguageCompiler;
    explicit LanguageIndex(config::Catalog catalog);
    struct Atom { std::string action; std::vector<std::string> tokens; };
    const runtime::DomainView domain_;
    std::map<std::string,std::vector<Atom>> actions_;
    std::vector<config::LanguageConflict> conflicts_;
    std::string fingerprint_;
    std::size_t bytes_ = 0;
};
class LanguageCompiler final {
public:
    static std::shared_ptr<const LanguageIndex> compile(config::Catalog catalog);
};
}
