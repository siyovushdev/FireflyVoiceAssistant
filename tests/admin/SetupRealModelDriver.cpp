// Local model bridge for reproducible qualitative checks. Parent supplies real
// model output through stdin; RuntimeWorker and document validation are unchanged.
#include "firefly/admin/RuntimeWorker.h"
#include <nlohmann/json.hpp>
#include <iostream>
using Json=nlohmann::json;
class LocalModel final : public firefly::admin::ContextModel {
    std::string prompt_;int budget_=768;
public:
    bool setupRequired() const override {return true;}
    bool ready() const override {return !prompt_.empty();}
    void enterSetup() override {}
    void useContext(const std::string& text) override {prompt_=text;}
    void useAdminContext(const std::string& text,int budget) override {prompt_=text;budget_=budget;}
    void leaveSetup(const std::string& text) override {useContext(text);}
    std::string infer(std::string_view text) override {
        std::cout<<Json{{"event","model_request"},{"system",prompt_},{"input",text},{"budget",budget_}}.dump()<<std::endl;
        std::string reply;if(!std::getline(std::cin,reply))throw std::runtime_error("model bridge closed");
        return Json::parse(reply).at("raw").get<std::string>();
    }
    std::string inferenceMetrics() const override {return R"({"backend":"local_Qwen3_4B_GGUF_CPU"})";}
};
int main() {
    try {
        std::string line;if(!std::getline(std::cin,line))return 2;const auto job=Json::parse(line);
        LocalModel model;firefly::admin::RuntimeWorker worker(model);
        auto applied=Json::parse(worker.handle(Json{{"method","apply"},{"configuration",job.at("configuration")}}.dump()));
        if(!applied.value("ok",false))throw std::runtime_error(applied.dump());
        auto entered=Json::parse(worker.handle(R"({"method":"setup_enter"})"));
        if(!entered.value("ok",false))throw std::runtime_error(entered.dump());
        auto result=Json::parse(worker.handle(Json{{"method","dialogue"},{"configuration",job.at("configuration")},{"text",job.at("text")},{"focus",Json::array()}}.dump()));
        std::cout<<Json{{"event","result"},{"result",result}}.dump()<<std::endl;
        return 0;
    } catch(const std::exception& e) {std::cout<<Json{{"event","failure"},{"error",e.what()}}.dump()<<std::endl;return 1;}
}
