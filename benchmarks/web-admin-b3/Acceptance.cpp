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
    if(argc!=2)return 2;
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
                else if(correct && label=="no-zone") correct=config.at("rooms").empty() && config.at("devices").size()==1 && config.at("devices")[0].at("room_id").is_null() && config.at("devices")[0].at("action_ids").empty();
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
        message("A","Создай Детскую, добавь туда Свет и Телевизор. Оба только включаются и выключаются.");
        if(state.value("message_incomplete",false))throw std::runtime_error("A_incomplete_inspect_trace_before_further_runs");
        const auto empty=Json::parse(baseline->canonicalJson());reset(empty);
        // Test fixture creates only the existing Zone for scenario B. Device is real AI.
        auto zoneRaw=expandDialogueOperations(R"({"ops":[["location","Цех упаковки"]]})");
        auto workshop=Json::parse(decodeCopilotProposal(zoneRaw.dump(),baseline)).at("configuration");reset(workshop);
        message("B","Зона: Цех упаковки . Добавь вентилятор, только включение и выключение.");
        if(state.value("message_incomplete",false))throw std::runtime_error("B_incomplete_inspect_trace_before_further_runs");
        auto devices=state.at("configuration").at("devices");
        if(devices.empty())throw std::runtime_error("B_no_device_requires_owner_review");
        const auto candidate=state.at("configuration");const auto target=devices[0].at("id").get<std::string>();
        const Json facts={{"endpoint","http://127.0.0.1:8765"},{"on",{{"method","POST"},{"path","/relay/1"},{"body",{{"on",true}}}}},{"off",{{"method","POST"},{"path","/relay/1"},{"body",{{"on",false}}}}},{"observe","/relay/1"},{"field","on"},{"noObserve",false}};
        const std::string physical="Настрой только HTTP подключение для Вентилятор ("+target+"). Создать отдельное подключение только для выбранного устройства.\nЯвные факты пользователя: "+facts.dump()+". Не меняй семантику. ON/OFF — существующие действия питания, Observe относится к существующему boolean свойству power. Никаких других сетевых фактов.";
        result=call({{"method","copilot"},{"configuration",candidate},{"text",physical}});
        report["cases"].push_back({{"case","C"},{"text",physical},{"result",result},{"classification",result.value("ok",false)?"proposal_requires_review":"validation_rejection"}});save();
        if(!result.value("ok",false))throw std::runtime_error("C_failed_inspect_context_output_budget_before_changes");
        reset(Json::parse(acceptCopilotProposal(candidate.dump(),result.at("draft").dump())).at("configuration"));
        result=call({{"method","copilot"},{"configuration",candidate},{"text","Подключи насос к контроллеру. Как обычно."}});
        report["cases"].push_back({{"case","D"},{"result",result},{"classification",result.value("ok",false)?"wrong_proposal":"clarification_or_rejection"}});save();
        result=call({{"method","copilot"},{"configuration",state.at("configuration")},{"text","Переименуй Вентилятор в Вытяжку и одновременно измени HTTP подключение, адрес как обычно."}});
        report["cases"].push_back({{"case","E-mixed-missing-facts"},{"result",result},{"classification",result.value("ok",false)?"wrong_proposal":"validation_rejection_or_clarification"}});save();
        message("F","Переименуй Вентилятор в зоне упаковки в Вытяжку.");
        reset(empty);message("no-zone","Добавь контроллер стенда без зоны.");
        for(const auto* name:{"Цех","Склад","Лабораторный стенд"}) {reset(empty);message(name,std::string("Создай зону ")+name+".");}
        report["cancel"]=setup.handle("cancel",{{"session_id",token}});report["stored_unchanged"]=store.current()->contentHash()==baseline->contentHash();save();
        worker.stop();return 0;
    }catch(const std::exception& error){report["stopped"]=error.what();save();std::cerr<<error.what()<<'\n';return 1;}
}
