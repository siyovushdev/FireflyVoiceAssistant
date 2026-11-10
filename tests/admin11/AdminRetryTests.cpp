#include "../admin/Support.h"
#include "firefly/admin/AdminDraft.h"
#include <iostream>

using namespace admin_test;
namespace {
int checks = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
class ScriptedModel final : public Model {
public:
    std::vector<std::string> responses, prompts;
    std::vector<int> budgets;
    std::size_t index = 0;
    bool rejectRetryContext = false;
    bool timeout = false;
    void useAdminContext(const std::string& value, int budget) override {
        if (rejectRetryContext && value.find("ПОЛНЫЙ корректный JSON")!=std::string::npos)
            throw std::runtime_error("Injected retry context exceeds export budget");
        useContext(value); prompts.push_back(value); budgets.push_back(budget);
    }
    std::string infer(std::string_view) override {
        ++inferences;
        if(timeout)throw std::runtime_error("RKLLM deadline exceeded; abort status=0");
        if (index >= responses.size()) throw std::runtime_error("Unexpected extra inference");
        return responses[index++];
    }
    std::string inferenceMetrics() const override {
        return Json{{"output_tokens",162},{"output_budget",budgets.back()},
            {"wall_ms",12},{"finish","vendor_finish_below_limit"}}.dump();
    }
    void script(std::vector<std::string> values) { responses=std::move(values); index=0; prompts.clear(); budgets.clear(); }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto baseline = base(argv[1]);
        ScriptedModel model; firefly::admin::RuntimeWorker worker(model);
        firefly::admin::LocalRuntimeClient client(worker); Directory directory;
        firefly::config::ConfigStore store(directory.path); store.initialize(catalog(baseline));
        firefly::admin::Api api(store,client); api.initializeRuntime();
        const auto call = [&](const std::string& path, Json body) {
            const auto response=api.handle("POST",path,body.dump());
            return std::pair<int,Json>{response.status,Json::parse(response.body)};
        };
        const auto originalHash=store.current()->contentHash();
        const auto state = [&] {
            return Json{{"devices",Json::parse(api.handle("GET","/api/devices/state","").body).at("devices")},
                {"history",Json::parse(api.handle("GET","/api/executions","").body).at("executions")},
                {"active",Json::parse(api.handle("GET","/api/status","").body).at("active")}};
        };
        const auto originalState=state();
        const auto commandContext=model.prompt;
        const auto unchanged = [&] {
            check(store.current()->contentHash()==originalHash,"AI must not save");
            check(state()==originalState,"AI must not Apply or execute");
            check(model.prompt==commandContext,"Command context restored");
        };
        const std::string plan=R"({"location":{"name":"Зона упаковки","type":"production_area","description":"Место упаковки.","mentions":["упаковки"],"locations":["в зоне упаковки"]},"devices":[{"name":"Освещение упаковки","template":"light","aliases":["свет упаковки"],"description":"Освещение зоны."},{"name":"Конвейер 1","template":"conveyor","aliases":["конвейер 1"],"description":"Перемещение изделий."},{"name":"Конвейер 2","template":"conveyor","aliases":["конвейер 2"],"description":"Перемещение изделий."},{"name":"Вентилятор упаковки","template":"switch","aliases":["вентилятор упаковки"],"description":"Виртуальная вентиляция."}]})";
        const Json request{{"text","Добавь зону упаковки, освещение, два конвейера и вентилятор"},{"configuration",baseline}};
        model.script({plan+"}",plan});
        auto reply=call("/api/admin/plan",request);
        check(reply.first==200 && reply.second.at("retry_count")==1,"Malformed then valid succeeds once");
        const auto trace=reply.second.at("trace");
        check(trace.size()==2 && trace[0].at("raw")==plan+"}" && trace[1].at("raw")==plan,"Raw attempts retained without repair");
        check(trace[0].contains("error") && trace[1].contains("decoded"),"Both strict decode outcomes retained");
        check(trace[0].at("metrics").at("output_tokens")==162 && trace[0].at("metrics").at("output_budget")==384,"Malformed below budget is not truncation");
        check(model.prompts[1].find("ПОЛНЫЙ корректный JSON")!=std::string::npos,"Explicit complete-schema retry instructions");
        check(model.prompts[1].find(trace[0].at("error").get<std::string>())!=std::string::npos,"Retry prompt includes concrete rejection reason");
        check(reply.second.at("draft").at("configuration").at("devices").size()==9,"Four devices deterministically expanded");
        auto graph=reply.second.at("draft").at("configuration");
        check(catalog(graph)->configuration().schemaVersion==2,"Expanded plan passes actual config validation");
        check(graph.at("actions").size()==12,"Two conveyors and switch expand to ten declarations plus legacy actions");
        check(reply.second.at("saved")==false && reply.second.at("applied")==false,"Explicit proposal markers"); unchanged();
        const std::string officePlan=R"({"location":{"name":"Кабинет 1","type":"office","description":"Офисный кабинет.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},"devices":[{"name":"Свет кабинета 1","template":"light","aliases":["свет кабинета 1"],"description":"Освещение кабинета."},{"name":"Телевизор кабинета 1","template":"tv","aliases":["телевизор кабинета 1"],"description":"Экран кабинета."},{"name":"Кондиционер кабинета 1","template":"switch","aliases":["кондиционер кабинета 1"],"description":"Виртуальный климат."}]})";
        model.script({officePlan});
        reply=call("/api/admin/plan",{{"text","Создай офис, кабинет 1. В кабинете добавь свет, телевизор и кондиционер."},{"configuration",baseline}});
        check(reply.first==200 && reply.second.at("retry_count")==0,"Natural office plan validates without retry");
        check(reply.second.at("draft").at("configuration").at("rooms").size()==5,"Office location added once");
        check(reply.second.at("saved")==false && reply.second.at("applied")==false,"Office plan stays draft");unchanged();
        const std::string entranceAsSwitch=R"({"location":{"name":"Кабинет 1","type":"office","description":"Офисный кабинет.","mentions":["кабинет 1"],"locations":["в кабинете 1"]},"devices":[{"name":"Входная дверь","template":"switch","aliases":["входная дверь"],"description":"Дверь на входе."}]})";
        model.script({entranceAsSwitch,entranceAsSwitch});
        reply=call("/api/admin/plan",{{"text","Добавь входную дверь в кабинет 1."},{"configuration",baseline}});
        check(reply.first==422 && !reply.second.contains("draft"),"Entrance door as switch is rejected without a draft");
        check(reply.second.at("trace").size()==2 &&
            reply.second.at("trace")[0].at("error").get<std::string>().find("Protected entrance door")!=std::string::npos &&
            reply.second.at("trace")[1].at("error").get<std::string>().find("Protected entrance door")!=std::string::npos,
            "Controlled unsupported reason for protected entrance door");unchanged();
        for (const auto& item : Json::parse(firefly::admin::templatesJson())) {
            auto input=Json::parse(plan); input["devices"]=Json::array({input["devices"][0]});
            input["devices"][0]["template"]=item.at("id");
            if (item.at("id")=="door") {
                bool rejected=false;
                try { (void)firefly::admin::decodePlan(input.dump(),catalog(baseline)); } catch (const std::exception&) { rejected=true; }
                check(rejected,"Protected door cannot gain an alternate template binding");
            } else {
                const auto expanded=Json::parse(firefly::admin::decodePlan(input.dump(),catalog(baseline)));
                check(catalog(expanded.at("configuration"))->configuration().devices.size()==6,"Every available template produces a valid graph");
            }
        }
        model.script({plan+"}","```json\n"+plan+"\n```"});reply=call("/api/admin/plan",request);
        check(reply.first==422 && reply.second.at("trace").size()==2 && !reply.second.contains("draft"),"Retry exhaustion rejects complete proposal");
        check(model.index==2 && reply.second.at("retry_limit")==1,"Bounded two attempts");unchanged();
        model.script({"{"});model.rejectRetryContext=true;reply=call("/api/admin/plan",request);model.rejectRetryContext=false;
        check(reply.first==422 && reply.second.at("trace").size()==2,"Retry admission failure preserves first malformed response");
        check(reply.second.at("trace")[1].at("stage")=="context_admission" && reply.second.at("trace")[1].at("metrics").is_null(),"No invented inference metrics for a skipped stage");
        check(model.index==1 && reply.second.at("errors")[0].at("code")=="runtime_rejected","Context error never causes an additional inference retry");unchanged();
        const std::string identity=R"({"description":"Комната для детей.","type":"room"})";
        const std::string language=R"({"name":"детская комната","name_location":"в детской комнате","short":"детская","short_location":"в детской","synonym":"комната для детей","synonym_location":"в комнате для детей"})";
        model.script({"{",identity,language});reply=call("/api/admin/draft",{{"entity","location"},{"name","Детская комната"}});
        check(reply.first==200 && reply.second.at("trace").size()==3,"Grouped draft retries failed group only");
        check(reply.second.at("suggestion").at("mentions").size()==3 && !reply.second.at("suggestion").at("description").empty(),"Complete description and language draft");
        check(model.budgets==std::vector<int>({256,256,256}),"Entity task budget stays separate");unchanged();
        model.script({"{",identity,"{"});reply=call("/api/admin/draft",{{"entity","location"},{"name","Детская комната"}});
        check(reply.first==422 && reply.second.at("trace").size()==3 && !reply.second.contains("suggestion"),"Request-wide retry allowance and no partial draft");unchanged();
        auto tooMany=Json::parse(plan);tooMany["devices"].push_back(tooMany["devices"][0]);
        model.script({tooMany.dump(),tooMany.dump()});reply=call("/api/admin/plan",request);
        check(reply.first==422,"Oversized multi-entity plan remains bounded");unchanged();
        const auto generic=std::find_if(graph["devices"].begin(),graph["devices"].end(),[](const auto& d){return d.at("name")=="Вентилятор упаковки";});
        const std::string action=R"({"description":"Включить виртуальный вентилятор.","aliases":["включи"],"assignments":[{"field":"power","value":true}]})";
        model.script({"{",action});reply=call("/api/admin/action-draft",{{"name","Включить"},{"target",generic->at("id")},{"configuration",graph}});
        check(reply.first==200 && reply.second.at("suggestion").at("parameters").empty(),"Action draft strict retry succeeds");
        check(reply.second.at("suggestion").at("assignments")[0].at("value")==true,"Typed fixed effect preserved");unchanged();
        model.script({});model.timeout=true;const auto beforeTimeout=model.inferences;
        reply=call("/api/admin/plan",request);model.timeout=false;
        check(reply.first==422&&reply.second.at("errors")[0].at("code")=="ai_timeout","Model deadline has a specific user-visible failure");
        check(model.inferences==beforeTimeout+1&&!reply.second.contains("draft"),"Timeout never retries or accepts a partial draft");unchanged();
        ScriptedModel oneAttempt;firefly::admin::RuntimeGenerations limited(oneAttempt);limited.apply(catalog(baseline));
        oneAttempt.script({"{","{}"});
        const auto noRetry=Json::parse(limited.structuredDocumentSuggestion("Return JSON","test",384,
            [](std::string_view raw){return Json::parse(raw).dump();},0));
        check(!noRetry.at("ok").get<bool>()&&oneAttempt.index==1,"Exhausted shared document retry budget performs exactly one attempt");
        check(noRetry.at("retry_count")==0&&noRetry.at("retry_limit")==0,"Zero remaining retries has accurate accounting");
        model.available=false;reply=call("/api/admin/draft",{{"entity","device"},{"name","ТВ для детей"}});
        check(reply.first==422 && reply.second.at("errors")[0].at("code")=="ai_unavailable","Unavailable model does not retry");
        std::cout<<"PASS "<<checks<<" admin retry/complete draft checks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1; }
}
