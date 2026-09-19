#pragma once
#include "TestSupport.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
namespace runtime_test {
using namespace firefly;
using namespace firefly::actions;
inline int checks = 0;
inline void check(bool value, const std::string& label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
inline bool unsupported(const CandidateResult& value) { return std::holds_alternative<Unsupported>(value); }
inline const GenericAction& action(const CandidateResult& value) {
    check(!unsupported(value), "expected semantic GenericAction");
    return std::get<GenericAction>(value);
}
inline GenericAction light(const std::string& target, std::int64_t value) {
    return GenericAction("action_set_brightness", target, {{"brightness", IntegerArgument(value)}});
}
struct FakeLlm : LlmEngine {
    std::string raw = "U", original;
    int calls = 0;
    std::string infer(std::string_view text) override { ++calls; original = text; return raw; }
};
struct Runtime {
    runtime::DomainView domain;
    DeterministicParser parser;
    CompactV1Adapter codec;
    FakeLlm llm;
    CommandEngine engine;
    explicit Runtime(config::Catalog snapshot) : domain(std::move(snapshot)), parser(domain), codec(domain.snapshot()),
        engine(llm, parser, domain) {}
};
inline void invalidConfig(const config::Configuration& cfg) {
    bool failed = false;
    try { (void)config::ConfigValidator::validate(cfg); } catch (const config::ConfigError&) { failed = true; }
    check(failed, "invalid configuration must not publish a snapshot");
}
inline int finish(const char* suite) {
    std::cout << suite << ": " << checks << " typed semantic/compatibility checks passed\n";
    return 0;
}
}
