#include "firefly/ActionValidator.h"
#include "firefly/CommandEngine.h"
#include "firefly/DeterministicParser.h"
#include "firefly/SafetyGate.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/runtime/RuntimeConfig.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <type_traits>

namespace {
using namespace firefly;
int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
class FakeLlm final : public LlmEngine {
public:
    int calls = 0;
    std::string raw = "U", original;
    std::string infer(std::string_view input) override {
        ++calls;
        original = input;
        return raw;
    }
};
class CountedDomain final : public LightBrightnessDomain {
public:
    explicit CountedDomain(const runtime::DomainView& view) : view_(view) {}
    mutable int calls = 0;
    bool supportsBrightness(char code, std::int64_t value) const override {
        ++calls;
        return view_.supportsBrightness(code, value);
    }
private:
    const runtime::DomainView& view_;
};
class MissingDomain final : public LightBrightnessDomain {
public:
    bool supportsBrightness(char, std::int64_t) const override { return false; }
};
class FailedDomain final : public LightBrightnessDomain {
public:
    bool supportsBrightness(char, std::int64_t) const override { throw std::runtime_error("domain lookup failed"); }
};
std::string compact(char code, int value) {
    return "L|" + std::string(1, code) + "|" + std::to_string(value);
}
config::Catalog withBounds(const config::Catalog& sample, int minimum, int maximum) {
    auto changed = sample->configuration();
    for (auto& action : changed.actions) if (action.operation == config::Operation::SetBrightness) {
        action.parameters[0].minimum = minimum;
        action.parameters[0].maximum = maximum;
    }
    return config::ConfigValidator::validate(changed);
}
void requireMismatch(const runtime::ShadowComparison& comparison) {
    bool threw = false;
    try { comparison.requireEquivalent(); } catch (const std::runtime_error&) { threw = true; }
    check(threw, "mismatch assertion must fail loudly");
}
static_assert(std::is_base_of_v<LightBrightnessDomain, runtime::DomainView>);
static_assert(!std::is_copy_assignable_v<runtime::DomainView>);
static_assert(!std::is_copy_assignable_v<runtime::RuntimeConfig>);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Repository root required");
        const std::string root = argv[1];
        const runtime::RuntimeConfig owner(root + "/config/current-m4.json");
        const auto sample = owner.domain().snapshot();
        owner.comparison().requireEquivalent();
        owner.brightnessComparison().requireEquivalent();
        check(owner.comparison().checks == 1364, "old baseline shadow probes retained");
        check(owner.brightnessComparison().checks == 668, "complete new range-only probes");
        const auto canonical = sample->canonicalJson();
        const std::vector<std::pair<std::string, char>> locations{
            {"на кухне", 'K'}, {"в спальне", 'B'}, {"в гостиной", 'G'},
            {"в зале", 'G'}, {"в коридоре", 'C'}};

        for (const auto& limits : std::vector<std::pair<int, int>>{{0,100}, {10,90}, {0,0}, {100,100}, {50,50}}) {
            const auto [minimum, maximum] = limits;
            const auto snapshot = withBounds(sample, minimum, maximum);
            const runtime::DomainView domain(snapshot);
            const DeterministicParser parser(domain);
            parser.requireLegacyRoomDomain();
            CountedDomain counted(domain);
            FakeLlm llm;
            CommandEngine engine(llm, parser, counted);
            const auto comparison = runtime::compareLegacyBrightness(domain);
            check(comparison.checks == 668, "same probe coverage for every valid range");
            check(comparison.mismatches == runtime::compareLegacyBrightness(domain).mismatches,
                  "deterministic ordered shadow diagnostics");
            if (minimum == 0 && maximum == 100) comparison.requireEquivalent();
            else requireMismatch(comparison);
            check(comparison.mismatches.size() == static_cast<std::size_t>(4 * (100 - maximum + minimum)),
                  "exact range differences for four configured lights");
            for (const auto& [location, code] : locations) {
                check(domain.lightBrightnessBounds(code) == std::make_pair<std::int64_t,std::int64_t>(minimum, maximum),
                      "shared configured action bounds reach each current target");
                for (int value = -1; value <= 101; ++value) {
                    const bool supported = value >= minimum && value <= maximum;
                    check(domain.supportsBrightness(code, value) == supported, "all domain values and adjacent boundaries");
                    if (value < 0 || value > 100) continue;
                    const auto candidate = compact(code, value);
                    const auto expected = supported ? candidate : "U";
                    // Parser/codec recognize percentages independently of installation support.
                    const auto text = location + " свет " + std::to_string(value);
                    check(parser.parseChecked(text) == candidate, "frozen numeric parser envelope unchanged");
                    check(ActionValidator::validate(candidate).has_value(), "compact syntax remains independent");
                    const int before = llm.calls, domainBefore = counted.calls;
                    const auto fast = engine.process(text);
                    check(fast.compact() == expected && fast.path == CommandPath::FastPath && llm.calls == before,
                          "configured range applies to fast path without retry or clamp");
                    check(counted.calls == domainBefore + 1, "one shared domain check for a safe light candidate");
                    check(fast.error.empty() == supported, "domain miss is observable");

                    const auto fallbackText = "Пожалуйста, " + location + " поставь свет на " + std::to_string(value) + " процентов";
                    llm.raw = candidate;
                    const auto fallback = engine.process(fallbackText);
                    check(fallback.compact() == expected && fallback.path == CommandPath::LlmFallback && llm.calls == before + 1,
                          "same configured range applies after one fallback call");
                    check(fallback.rawOutput == candidate && llm.original == fallbackText,
                          "no raw/original repair");
                    check(counted.calls == domainBefore + 2, "one range decision on fallback too");
                    check(fallback.error.empty() == supported, "fallback range miss diagnosed");
                }
                for (const auto& [suffix, value] : std::vector<std::pair<std::string,int>>{
                         {"выключи свет ",0}, {"включи свет ",100}, {"полностью выключи свет ",0}}) {
                    const auto result = engine.process(suffix + location);
                    check(result.compact() == (value >= minimum && value <= maximum ? compact(code,value) : "U"),
                          "OFF and ON are not redefined by configured endpoints");
                }
                for (const auto& [state, value] : std::vector<std::pair<std::string,int>>{{"на максимум",100}, {"наполовину",50}})
                    check(engine.process(location + " свет " + state).compact() ==
                          (value >= minimum && value <= maximum ? compact(code,value) : "U"), "maximum/half never remapped");
                for (const int value : {0,1,50,100}) {
                    llm.raw = compact(code,value);
                    check(engine.process("Включи свет " + location + " на " + std::to_string(value) + " процентов").compact() ==
                          (value > 0 && value >= minimum && value <= maximum ? llm.raw : "U"), "ON/nonzero intersection preserved");
                    for (const auto& raw : {std::string("U"), compact(code,0), compact(code,50)}) {
                        llm.raw = raw;
                        const int domainBefore = counted.calls;
                        check(engine.process(location + " выключи свет на 50 процентов").compact() == "U",
                              "OFF contradiction cannot be overridden by either request interpretation");
                        check(counted.calls == domainBefore, "contradiction rejected by unchanged safety before domain decision");
                    }
                }
            }
            std::ifstream dev(root + "/tests/data/dev.tsv");
            check(bool(dev), "frozen fixtures readable");
            std::string line;
            int contradictions = 0;
            while (std::getline(dev, line)) if (line.find("\tcontradiction\tU\t") != std::string::npos) {
                ++contradictions;
                const auto original = line.substr(line.find("\tU\t") + 3);
                for (const char code : std::string("KBGC")) for (const int value : {0,1,20,40,50,80,100}) {
                    llm.raw = compact(code,value);
                    check(engine.process(original).compact() == "U", "all frozen contradictions survive range cutover");
                }
            }
            check(contradictions == 4, "all four original contradiction fixtures exercised");
            for (const auto* raw : {"U", "", "L|K|", "L|K|-1", "L|K|101", "L|K|1.5", "L|K|+40", "L|X|40", "L|K|40|extra"}) {
                llm.raw = raw;
                check(engine.process("Пожалуйста, на кухне поставь свет на 40 процентов").compact() == "U",
                      "malformed/out-of-protocol candidates never become configured actions");
            }
            llm.raw = "  L | K | 0 5 0 \n";
            check(engine.process("Пожалуйста, на кухне свет 50 процентов").compact() ==
                  (minimum <= 50 && maximum >= 50 ? "L|K|50" : "U"), "legacy whitespace and leading-zero decoding retained");
            llm.raw = "D";
            const int domainBefore = counted.calls;
            check(engine.process("Открой входную дверь").compact() == "D", "door output unchanged");
            check(counted.calls == domainBefore, "door does not query brightness consumer");
            for (const auto* input : {"свет 40", "в детской свет 40", "на кухне свет", "на кухне свет -1",
                     "на кухне свет 101", "на кухне свет 40,5", "на кухне свет 40 или 50", "на кухне и в спальне свет 40"}) {
                llm.raw = "U";
                check(engine.process(input).compact() == "U", "unsupported input unchanged with U fallback");
            }
        }

        for (int byte = 0; byte <= 255; ++byte) {
            const char code = static_cast<char>(static_cast<unsigned char>(byte));
            const bool known = std::string("KBGC").find(code) != std::string::npos;
            check(owner.domain().lightBrightnessBounds(code).has_value() == known, "complete supported target-code space");
            for (const std::int64_t value : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, std::int64_t{101},
                                             std::numeric_limits<std::int64_t>::max()})
                check(!owner.domain().supportsBrightness(code,value), "signed extremes reject without overflow");
            if (!known) check(!owner.domain().supportsBrightness(code,40), "unsupported target never receives default range");
        }
        const DeterministicParser parser(owner.domain());
        FakeLlm llm;
        llm.raw = "L|K|40";
        MissingDomain missing;
        FailedDomain failed;
        for (const LightBrightnessDomain* view : {static_cast<const LightBrightnessDomain*>(&missing),
                                                 static_cast<const LightBrightnessDomain*>(&failed)}) {
            CommandEngine engine(llm, parser, *view);
            const int before = llm.calls;
            const auto fast = engine.process("на кухне свет 40");
            check(fast.compact() == "U" && !fast.error.empty() && llm.calls == before, "domain miss/failure has no legacy or LLM repair");
            const auto fallback = engine.process("Пожалуйста, на кухне свет 40 процентов");
            check(fallback.compact() == "U" && !fallback.error.empty() && llm.calls == before + 1,
                  "domain failure after fallback is terminal");
        }
        // Range-only means permission fields cannot accidentally become consumers.
        auto restricted = sample->configuration();
        for (auto& device : restricted.devices) device.enabled = false;
        for (auto& action : restricted.actions) action.enabled = false;
        restricted.restrictions.push_back({{"test_deny", "Запрет"}, "device_kitchen_light", "action_set_brightness"});
        const runtime::DomainView permissionShadow(config::ConfigValidator::validate(restricted));
        runtime::compareLegacyBrightness(permissionShadow).requireEquivalent();
        check(!permissionShadow.allowsBrightness("device_kitchen_light",40), "existing permission-aware shadow view unchanged");
        const DeterministicParser unchangedRooms(permissionShadow);
        CommandEngine rangeOnly(llm, unchangedRooms, permissionShadow);
        check(rangeOnly.process("на кухне свет 40").compact() == "L|K|40", "enablement/deny not migrated along with range");
        llm.raw = "D";
        check(rangeOnly.process("Открой входную дверь").compact() == "D", "door configuration still observational");

        for (const auto& limits : std::vector<std::pair<int,int>>{{-1,100}, {0,101}, {90,10}}) {
            bool invalid = false;
            try { withBounds(sample,limits.first,limits.second); } catch (const config::ConfigError&) { invalid = true; }
            check(invalid, "invalid bounds cannot construct a snapshot; schema unchanged");
        }
        check(sample->canonicalJson() == canonical && owner.domain().supportsBrightness('K',0) && owner.domain().supportsBrightness('K',100),
              "test-only snapshots never mutate current owner");
        check(config::ConfigLoader::fromFile(root + "/config/current-m4.json")->canonicalJson() == canonical,
              "production sample remains unchanged");
        std::cout << "BrightnessCutover: " << checks << " checks passed; five ranges, four rooms/hall, both routes, safety and no fallback; 668 current shadow probes, zero mismatches\n";
    } catch (const std::exception& error) {
        std::cerr << "BrightnessCutover failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
