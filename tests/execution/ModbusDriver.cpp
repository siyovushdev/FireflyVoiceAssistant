#include "firefly/config/ConfigLoader.h"
#include "firefly/execution/ActionExecutor.h"
#include "firefly/actions/ScalarJson.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <dirent.h>
#include <thread>
#include <atomic>
using Json=nlohmann::json;
int descriptors() {int n=0;if(auto* d=::opendir("/proc/self/fd")){while(::readdir(d))++n;::closedir(d);}return n;}
int main() {
    try {
        Json input;std::cin>>input;
        const auto catalog=firefly::config::ConfigLoader::fromJson(input.at("configuration").dump());
        firefly::execution::BoundDeviceExecutor executor(catalog);
        const auto before=descriptors();Json output=Json::array();
        const auto operate=[&](const Json& op) {
            const auto start=std::chrono::steady_clock::now();Json result;
            if(op.at("mode")=="poll") {
                const auto r=executor.poll("device_relay",op.value("timeout_ms",0));
                result={{"success",r.success},{"error",r.error},{"detail",r.detail}};
            } else if(op.at("mode")=="validate")result={{"success",true}};
            else {
                firefly::actions::GenericAction::Arguments args;
                const auto arguments=op.value("arguments",Json::object());
                for(const auto& p:arguments.items())args.emplace(p.key(),firefly::actions::decodeScalar(p.value()));
                const auto mode=op.at("mode")=="playground"?firefly::execution::ExecutionIntent::Playground:op.at("mode")=="none"?firefly::execution::ExecutionIntent::None:firefly::execution::ExecutionIntent::Production;
                const auto r=executor.execute({op.value("action",std::string("action_on")),"device_relay",args},mode);
                result={{"success",r.success},{"error",r.error},{"accepted",r.status==firefly::execution::ExecutionStatus::Accepted}};
            }
            const auto device=executor.production().device("device_relay");
            result["properties"]=Json::object();
            for(const auto& [name,p]:device->properties)result["properties"][name]={{"knowledge",p.knowledge==firefly::execution::StateKnowledge::Confirmed?"confirmed":p.knowledge==firefly::execution::StateKnowledge::Predicted?"predicted":"unknown"},{"value",p.value?firefly::actions::scalarJson(*p.value):Json(nullptr)}};
            result["wall_ms"]=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();return result;
        };
        for(const auto& op:input.at("operations")) {
            if(op.at("mode")!="parallel") {output.push_back(operate(op));continue;}
            const auto& items=op.at("operations");
            if(items.empty() || items.size()>8)throw std::invalid_argument("parallel fixture size");
            std::atomic<bool> start{false};std::vector<std::thread> threads;std::vector<Json> results(items.size());
            for(std::size_t i=0;i<items.size();++i)threads.emplace_back([&,i] {
                while(!start)std::this_thread::yield();
                std::this_thread::sleep_for(std::chrono::milliseconds(items[i].value("start_after_ms",0)));
                try {results[i]=operate(items[i]);}
                catch(const std::exception& e) {results[i]={{"fixture_error",e.what()}};}
            });
            start=true;for(auto& thread:threads)thread.join();
            output.push_back(Json{{"parallel",results}});
        }
        std::cout<<Json{{"ok",true},{"results",output},{"fd_delta",descriptors()-before},{"canonical",Json::parse(catalog->canonicalJson())}}.dump()<<'\n';return 0;
    } catch(const std::exception& e) {std::cout<<Json{{"ok",false},{"error",e.what()}}.dump()<<'\n';return 0;}
}
