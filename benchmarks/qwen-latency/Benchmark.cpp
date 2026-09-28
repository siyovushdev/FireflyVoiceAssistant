#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/RkllmEngine.h"
#include "firefly/llm/StructuredOutput.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace firefly;
struct Model : admin::ContextModel {
    std::unique_ptr<RkllmEngine> engine;
    std::string path, prompt;
    llm::CommandProtocol protocol = llm::CommandProtocol::Structured;
    llm::CommandProtocol commandProtocol() const override { return protocol; }
    double setup = 0;
    int switches = 0;
    explicit Model(std::string file) : path(std::move(file)) {}
    void useContext(const std::string& value) override {
        auto start = Clock::now();
        bool changed = value != prompt;
        if (!engine) {
            RkllmConfig config; config.modelPath=path; config.systemPrompt=value;
            config.verbose=true; config.requestTokenReserve=128;
            engine=std::make_unique<RkllmEngine>(config);
        } else engine->setSystemPrompt(value);
        prompt=value;
        if(changed) { setup += std::chrono::duration<double,std::milli>(Clock::now()-start).count(); ++switches; }
    }
    bool ready() const override { return engine && engine->ready(); }
    int contextTokens() const override { return engine ? engine->contextTokens() : -1; }
    std::string inferCommand(std::string_view original, std::string_view contract) override { return engine->inferCommand(original, contract); }
    std::string infer(std::string_view original) override { return engine->infer(original); }
};
int main(int argc,char** argv) { try {
    if(argc!=6) throw std::runtime_error("Usage: benchmark MODEL CONFIG CORPUS OUTPUT MODE");
    std::ifstream in(argv[3]); const auto cases=Json::parse(in);
    const auto catalog = config::ConfigLoader::fromFile(argv[2]);
    // Independently verify the selected IDs against production grounding before
    // touching the NPU. Corpus labels never select a model target or action.
    admin::RuntimeGeneration probe(catalog);
    for (const auto& item : cases) {
        const auto prompt = probe.requestPrompt(item.at("input").get<std::string>());
        std::string key;
        for (const auto& entry : probe.targetContexts) if (entry.second == prompt) key = entry.first;
        const auto slash = key.find('/');
        const auto target = key.substr(0, slash);
        const auto* device = probe.domain.device(target);
        if (!device || device->actionIds.size() != 1 || target != item.at("target") ||
            device->actionIds.front() != item.at("action"))
            throw std::runtime_error("Corpus grounding differs from production selection");
    }
    if (std::string(argv[5]) == "check") { std::cout << "Corpus grounding verified\n"; return 0; }
    Model model(argv[1]);
    if (std::string(argv[5]) == "compact-scoped") model.protocol = llm::CommandProtocol::ScopedCompact;
    if (std::string(argv[5]) == "final") model.protocol = llm::CommandProtocol::StableCompact;
    admin::RuntimeGenerations runtime(model);
    runtime.apply(catalog);
    Json report{{"system_prompt",model.prompt},{"corpus",argv[3]},{"configuration",argv[2]},{"mode",argv[5]},{"startup_setup_ms",model.setup},{"startup_context_installs",model.switches},{"requests",Json::array()}};
    std::string previous;
    for(size_t i=0;i<cases.size();++i) {
        const auto& item=cases[i]; model.setup=0; model.switches=0;
        auto start=Clock::now(); auto result=runtime.process(item.at("input").get<std::string>());
        const double total=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        const auto actual=Json::parse(llm::structuredResult(result.command.action));
        const auto metrics=model.engine->metrics();
        const bool fallback=result.command.path==CommandPath::LlmFallback;
        const auto target=item.at("target").get<std::string>();
        Json row{{"index",i},{"input",item.at("input")},{"selected_target",target},{"selected_action",item.at("action")},
            {"category",i==0?"first":target==previous?"same_target":"target_switch"},
            {"context_bootstrap_ms",model.setup},{"context_installs",model.switches},
            {"prefill_ms",fallback?metrics.prefillMs:0},{"prefill_tokens",fallback?metrics.inputTokens:0},
            {"generation_ms",fallback?metrics.generationMs:0},{"output_tokens",fallback?metrics.outputTokens:0},
            {"inference_wall_ms",fallback?metrics.wallMs:0},{"total_llm_wall_ms",fallback?model.setup+metrics.wallMs:0},
            {"total_request_ms",total},{"fallback",fallback},{"raw",result.command.rawOutput},
            {"result",actual},{"expected",item.at("expected")},{"correct",actual==item.at("expected")},{"error",result.command.error}};
        report["requests"].push_back(row); previous=target;
        std::ofstream(argv[4])<<report.dump(2)<<'\n';
        std::cout<<i<<" fallback="<<fallback<<" correct="<<row["correct"]<<" setup="<<model.setup<<" total="<<total<<std::endl;
    }
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
