#include "firefly/CommandEngine.h"
#include "firefly/RkllmEngine.h"
#include "firefly/runtime/RuntimeConfig.h"
#include "firefly/llm/LlmContext.h"
#include "firefly/actions/CompactV1Adapter.h"
#include "CliSupport.h"
#include <iomanip>
#include <vector>

namespace {
struct Case { std::string id, category, expected, input; };
std::string escaped(std::string_view input) {
    std::string result;
    for (char c : input) {
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c == '\\') result += "\\\\";
        else result += c;
    }
    return result;
}
std::vector<Case> readCases(const std::string& path, const firefly::actions::CompactV1Adapter& codec) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open DEV dataset: " + path);
    std::vector<Case> cases;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        const auto c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos) throw std::runtime_error("Expected four TSV fields");
        Case test{line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1, c - b - 1), line.substr(c + 1)};
        if (std::holds_alternative<firefly::actions::CompactV1Error>(codec.decode(test.expected)))
            throw std::runtime_error("Invalid expected action: " + test.id);
        cases.push_back(std::move(test));
    }
    if (file.bad() || cases.empty()) throw std::runtime_error("Unreadable/empty dataset");
    return cases;
}
}
int main(int argc, char** argv) {
    try {
        firefly::RkllmConfig config;
        std::string data, configPath = firefly::runtime::RuntimeConfig::defaultPath();
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--verbose") { config.verbose = true; continue; }
            if (i + 1 == argc) throw std::runtime_error("Missing option value");
            if (arg == "--model") config.modelPath = argv[++i];
            else if (arg == "--config") configPath = argv[++i];
            else if (arg == "--data") data = argv[++i];
            else throw std::runtime_error("Unknown option: " + arg);
        }
        const firefly::runtime::RuntimeConfig runtime(configPath);
        const firefly::DeterministicParser parser(runtime.domain());
        const firefly::actions::CompactV1Adapter outputCodec(runtime.domain().snapshot());
        const auto cases = readCases(data, outputCodec);
        config.systemPrompt = firefly::llm::generateContext(runtime.domain()).systemPrompt();
        config.progress = [](std::string_view stage) { std::cerr << stage << std::endl; };
        firefly::cli::CliOutput output;
        firefly::RkllmEngine llm(std::move(config));
        firefly::CommandEngine engine(llm, parser, runtime.domain());
        int pass = 0, valid = 0, unsupported = 0, falseAccept = 0, falseReject = 0, wrong = 0;
        int invalid = 0, finalInvalid = 0, fast = 0, fallback = 0, inputRejected = 0, errors = 0;
        int structuredActions = 0, structuredUnsupported = 0, safetyRejects = 0, domainRejects = 0, semanticWrong = 0;
        int malformedJson = 0, invalidStructured = 0;
        double totalMs = 0, fastMs = 0, fallbackMs = 0;
        for (const auto& test : cases) {
            const auto result = engine.process(test.input);
            const auto final = outputCodec.render(result.action);
            const bool correct = final == test.expected;
            pass += correct;
            valid += correct && test.expected != "U";
            unsupported += correct && test.expected == "U";
            falseAccept += test.expected == "U" && final != "U";
            falseReject += test.expected != "U" && final == "U";
            wrong += test.expected != "U" && final != "U" && !correct;
            invalid += result.invalidModelOutput;
            malformedJson += result.decodeStatus == firefly::llm::DecodeStatus::InvalidJson;
            invalidStructured += result.invalidModelOutput && result.decodeStatus != firefly::llm::DecodeStatus::InvalidJson;
            structuredActions += result.decodeStatus == firefly::llm::DecodeStatus::Action;
            structuredUnsupported += result.decodeStatus == firefly::llm::DecodeStatus::Unsupported;
            safetyRejects += result.safetyRejected;
            domainRejects += result.domainRejected;
            const auto expectedSemantic = std::get<firefly::actions::CandidateResult>(outputCodec.decode(test.expected));
            semanticWrong += result.path == firefly::CommandPath::LlmFallback && !result.invalidModelOutput &&
                             result.candidate != expectedSemantic;
            finalInvalid += std::holds_alternative<firefly::actions::CompactV1Error>(outputCodec.decode(final));
            errors += !result.error.empty();
            totalMs += result.latencyMs;
            const char* route = "INPUT REJECTED";
            if (result.path == firefly::CommandPath::FastPath) { ++fast; fastMs += result.latencyMs; route = "FAST PATH"; }
            if (result.path == firefly::CommandPath::LlmFallback) { ++fallback; fallbackMs += result.latencyMs; route = "LLM FALLBACK"; }
            if (result.path == firefly::CommandPath::InputRejected) ++inputRejected;
            std::ostringstream row;
            row << test.id << '\t' << (correct ? "PASS" : "FAIL") << '\t' << route
                << "\texpected=" << test.expected << "\tfinal=" << final << "\tms=" << result.latencyMs << '\n';
            output.write(row.str());
            std::cerr << test.id << " input=[" << escaped(test.input) << "] raw=[" << escaped(result.rawOutput)
                      << "] decode=[" << firefly::llm::decodeStatusName(result.decodeStatus)
                      << "] candidate=[" << firefly::llm::structuredResult(result.candidate)
                      << "] safety=[" << firefly::llm::structuredResult(result.safeCandidate)
                      << "] semantic_final=[" << firefly::llm::structuredResult(result.action) << "] final=[" << final
                      << "] error=[" << escaped(result.error) << "]" << std::endl;
        }
        std::ostringstream summary;
        summary << "total=" << cases.size() << " pass=" << pass << " fail=" << cases.size() - pass
                << " valid_correct=" << valid << " unsupported_correct=" << unsupported
                << " false_accept=" << falseAccept << " false_reject=" << falseReject << " wrong_action=" << wrong
                << " invalid_output=" << invalid << " final_invalid_output=" << finalInvalid
                << " structured_actions=" << structuredActions << " structured_unsupported=" << structuredUnsupported
                << " malformed_json=" << malformedJson << " invalid_structured=" << invalidStructured
                << " decoder_rejects=" << invalid << " safety_rejects=" << safetyRejects
                << " domain_rejects=" << domainRejects << " llm_semantic_wrong=" << semanticWrong
                << " runtime_or_input_errors=" << errors << " fast_path=" << fast << " llm_fallback=" << fallback
                << " input_rejected=" << inputRejected << " average_ms=" << totalMs / cases.size()
                << " fast_average_ms=" << (fast ? fastMs / fast : 0)
                << " fallback_average_ms=" << (fallback ? fallbackMs / fallback : 0) << '\n';
        output.write(summary.str());
        return pass == static_cast<int>(cases.size()) && !finalInvalid && !errors ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Regression failed: " << error.what() << '\n';
        return 2;
    }
}
