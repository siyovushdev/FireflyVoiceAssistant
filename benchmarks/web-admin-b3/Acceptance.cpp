// Explicit live NPU gate, never a CTest and never a production ConfigStore.
#include "RuntimeModel.h"
#include "SetupSession.h"
#include "DialogueOperations.h"
#include "firefly/admin/RuntimeWorker.h"
#include "firefly/admin/AdminCopilot.h"
#include <filesystem>
#include <iostream>
#include <csignal>
using namespace firefly::admin;
volatile std::sig_atomic_t interrupted=0;
void stop(int) { interrupted=1; }
int main(int argc,char**argv) {
    const bool languageOnly=argc==3 && std::string(argv[2])=="--language-profiles";
    const bool noZoneOnly=argc==3 && std::string(argv[2])=="--no-zone";
    if(argc!=2 && !noZoneOnly && !languageOnly && !(argc==4 && std::string(argv[2])=="--remaining"))return 2;
    std::signal(SIGTERM,stop);std::signal(SIGINT,stop);
    std::ofstream("/proc/self/oom_score_adj")<<1000;
    Json report={{"cases",Json::array()},{"calls",Json::array()},{"hardware_requests",0},{"real_voice","not_run"}};
    const std::string output=argv[1];
    const auto save=[&]{std::ofstream(output)<<report.dump(2)<<'\n';};
    char path[]="/tmp/firefly-b3-npu-XXXXXX";const char* directory=::mkdtemp(path);if(!directory)return 2;
    report["private_store"]=directory;save();
    try {
        firefly::config::ConfigStore store(directory);
        const auto baseline=store.initialize(firefly::config::ConfigLoader::fromFile("config/empty-home.json"));
        RuntimeModel model("/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm");
        RuntimeWorker worker(model);
        const auto call=[&](const Json& request) {
            if(interrupted)throw std::runtime_error("cancelled");
            const auto begin=std::chrono::steady_clock::now();
            const auto reply=Json::parse(worker.handle(request.dump()));
            report["calls"].push_back({{"method",request.at("method")},{"input_bytes",request.value("text",std::string()).size()},
                {"wall_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()},
                {"context_tokens",model.contextTokens()},{"result",reply}});save();
            std::cout<<"CALL "<<request.at("method")<<" ok="<<reply.value("ok",false)<<std::endl;
            return reply;
        };
        auto result=call({{"method","apply"},{"configuration",Json::parse(baseline->canonicalJson())}});
        if(!result.value("ok",false))throw std::runtime_error("initial_admission_failed");
        SetupSession setup(store,call);auto state=setup.handle("start",{});const auto token=state.at("session_id");int seq=0;
        const auto reset=[&](const Json& config) {
            if(state.value("message_incomplete",false)) {
                setup.handle("cancel",{{"session_id",token}});
                throw std::runtime_error("message_incomplete_requires_owner_review");
            }
            state=setup.handle("draft",{{"session_id",token},{"version",state.at("version")},{"configuration",config}});
        };
        const auto message=[&](const std::string& label,const std::string& text) {
            const auto begin=std::chrono::steady_clock::now();
            Json record={{"case",label},{"text",text}};
            const auto before=state.at("configuration");
            try {
                state=setup.handle("message",{{"session_id",token},{"version",state.at("version")},{"request_id",std::to_string(++seq)},{"atomic_message",true},{"text",text}});
                bool correct=state.at("status")=="editing";
                const auto& config=state.at("configuration");
                if(correct && label=="A") {
                    correct=config.at("rooms").size()==1 && config.at("devices").size()==2;
                    std::set<std::string> names;
                    for(const auto& d:config.at("devices")) {names.insert(d.at("name"));correct=correct&&d.at("action_ids").size()==2;}
                    correct=correct&&names==std::set<std::string>{"Свет","Телевизор"};
                } else if(correct && label=="B") correct=config.at("devices").size()==1 && config.at("devices")[0].at("name")=="Вентилятор" && config.at("devices")[0].at("action_ids").size()==2;
                else if(correct && label=="no-zone") {
                    correct=config.at("rooms").empty() && config.at("devices").size()==1 &&
                        config.at("devices")[0].at("room_id").is_null() && config.at("devices")[0].at("action_ids").empty() &&
                        config.at("devices")[0].at("state").empty() && config.at("actions").empty();
                }
                else if(correct && label=="F") correct=config.at("devices")[0].at("id")==before.at("devices")[0].at("id") && config.at("devices")[0].at("name")=="Вытяжка" && config.at("actions")==before.at("actions") && config.at("bindings")==before.at("bindings");
                else if(correct && (label=="Цех" || label=="Склад" || label=="Лабораторный стенд")) correct=config.at("rooms").size()==1 && config.at("rooms")[0].at("name")==label && config.at("devices").empty();
                record["classification"]=state.at("status")!="editing"?"false_rejection":correct?"semantic_success":"wrong_proposal";record["state"]=state;
            } catch(const std::exception& error) {
                record["classification"]="validation_rejection";record["error"]=error.what();
                state=setup.handle("status",{});record["state"]=state;
            }
            record["wall_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            report["cases"].push_back(record);save();std::cout<<"CASE "<<label<<" "<<record["classification"]<<std::endl;
        };
        const auto empty=Json::parse(baseline->canonicalJson());
        Json candidate;
        // Explicit continuation runs only previously unexecuted cases. It never
        // retries the failed request or changes the model/profile.
        if(languageOnly) {
            report["scope"]="location_language_profiles";
            for(const auto* name:{"Цех","Кухня","Склад"}) {
                const auto ops=expandDialogueOperations(Json{{"ops",Json::array({Json::array({"location",name})})}}.dump());
                const auto config=Json::parse(decodeCopilotProposal(ops.dump(),baseline)).at("configuration");
                const auto reply=call({{"method","language_profile"},{"configuration",config},{"entity","location"},
                    {"id",config.at("rooms")[0].at("id")},{"initial_location",true}});
                Json record={{"case",name},{"result",reply},{"index_commands",Json::array()},
                    {"classification",reply.value("ok",false)?"validated_suggestion":"validation_rejection"}};
                if(reply.value("ok",false)) {
                    auto candidate=config;
                    const auto& profile=reply.at("suggestion");
                    candidate["rooms"][0]["speech"]={{"mentions",profile.at("mentions")},{"locations",profile.at("locations")}};
                    candidate["rooms"][0]["metadata"]["aliases"]=profile.at("aliases");
                    const auto device=expandDialogueOperations(Json{{"ops",Json::array({Json::array({"power_device",name,"Свет"})})}}.dump());
                    candidate=Json::parse(decodeCopilotProposal(device.dump(),firefly::config::ConfigLoader::fromJson(candidate.dump()))).at("configuration");
                    RuntimeGeneration generation(firefly::config::ConfigLoader::fromJson(candidate.dump()));
                    // Fixed acceptance corpus, never a production morphology map.
                    const auto phrase=std::string(name)=="Цех"?"в цехе":std::string(name)=="Кухня"?"на кухне":"на складе";
                    for(const auto& text:{std::string("Включи свет ")+phrase,std::string("Выключи свет ")+phrase}) {
                        const auto match=generation.languageIndex->match(text);
                        const bool exact=match.kind==firefly::language::MatchKind::ExactUnique;
                        record["index_commands"].push_back({{"text",text},{"exact",exact}});
                        if(!exact)record["classification"]="index_coverage_failure";
                    }
                }
                report["cases"].push_back(record);save();
                if(!model.ready())throw std::runtime_error("language_model_unavailable_no_retry");
            }
        } else if(noZoneOnly) {
            report["scope"]="single_no_zone_retest";
            message("no-zone","Добавь контроллер стенда без зоны.");
        } else if(argc==4) {
            std::ifstream previousFile(argv[3]);Json previous;previousFile>>previous;
            for(const auto& record:previous.at("cases"))
                if(record.at("case")=="B" && record.at("classification")=="semantic_success")
                    candidate=record.at("state").at("configuration");
            if(candidate.is_null())throw std::runtime_error("remaining_requires_successful_B_fixture");
            candidate=Json::parse(firefly::config::ConfigLoader::fromJson(candidate.dump())->canonicalJson());
            report["continued_from"]=argv[3];save();
        } else {
            message("A","Создай Детскую, добавь туда Свет и Телевизор. Оба только включаются и выключаются.");
            if(state.value("message_incomplete",false))throw std::runtime_error("A_incomplete_inspect_trace_before_further_runs");
            reset(empty);
            // Fixture creates only the existing Zone. Device is real AI.
            auto zoneRaw=expandDialogueOperations(R"({"ops":[["location","Цех упаковки"]]})");
            auto workshop=Json::parse(decodeCopilotProposal(zoneRaw.dump(),baseline)).at("configuration");reset(workshop);
            message("B","Зона: Цех упаковки . Добавь вентилятор, только включение и выключение.");
            if(state.value("message_incomplete",false))throw std::runtime_error("B_incomplete_inspect_trace_before_further_runs");
            candidate=state.at("configuration");
            if(candidate.at("devices").empty())throw std::runtime_error("B_no_device_requires_owner_review");
            message("F","Переименуй Вентилятор в зоне упаковки в Вытяжку.");
            reset(empty);message("no-zone","Добавь контроллер стенда без зоны.");
        }
        if(!noZoneOnly && !languageOnly) {
            const auto target=candidate.at("devices")[0].at("id").get<std::string>();
            // Physical C uses the shared B2 path in browser/API tests, never the NPU.
            report["connectivity"] = {{"mode","deterministic_explicit_facts"},{"npu_calls",0}};
            for(const auto* name:{"Цех","Склад","Лабораторный стенд"}) {reset(empty);message(name,std::string("Создай зону ")+name+".");}
            const auto assist=[&](const std::string& label, const Json& request) {
                const auto reply=call(request);
                report["cases"].push_back({{"case",label},{"result",reply},
                    {"classification",reply.value("ok",false)?"validated_suggestion":"validation_rejection"}});
                save();
                if(!model.ready()) throw std::runtime_error(label+"_model_unavailable_no_retry");
            };
            assist("contextual-zone",{{"method","draft"},{"entity","location"},{"name","Цех"}});
            assist("contextual-device",{{"method","draft"},{"entity","device"},{"name","Вентилятор"}});
            assist("contextual-action-language",{{"method","language_profile"},{"configuration",candidate},
                {"entity","action"},{"id",candidate.at("devices")[0].at("action_ids")[0]}});
            assist("contextual-action-schema",{{"method","action_draft"},{"configuration",candidate},
                {"target",target},{"name","Включить"}});
        }
        report["cancel"]=setup.handle("cancel",{{"session_id",token}});report["stored_unchanged"]=store.current()->contentHash()==baseline->contentHash();save();
        bool passed=true;
        for(const auto& record:report["cases"]) {
            const auto classification=record.at("classification");
            passed=passed&&(classification=="semantic_success"||classification=="validated_suggestion");
        }
        if(noZoneOnly) {
            int dialogueCalls=0;
            for(const auto& entry:report["calls"])if(entry.at("method")=="dialogue") {
                ++dialogueCalls;
                passed=passed && entry.at("result").at("retry_count")==0 && entry.at("result").at("trace").size()==1;
            }
            passed=passed && dialogueCalls==1 && report.at("stored_unchanged").get<bool>();
        }
        report["passed"]=passed;save();worker.stop();return passed?0:1;
    }catch(const std::exception& error){report["stopped"]=error.what();save();std::cerr<<error.what()<<'\n';return 1;}
}
