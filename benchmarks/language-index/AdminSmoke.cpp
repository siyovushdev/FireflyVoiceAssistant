#include "RuntimeModel.h"
#include "firefly/admin/RuntimeWorker.h"
#include "firefly/config/ConfigLoader.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Json=nlohmann::json;
int main(int argc,char** argv){try{
    if(argc!=4)return 2;
    firefly::admin::RuntimeModel model(argv[1]);firefly::admin::RuntimeWorker worker(model);
    const auto catalog=firefly::config::ConfigLoader::fromFile(argv[2]);auto config=Json::parse(catalog->canonicalJson());
    for (auto& a : config["actions"]) if (a["id"] == "power_on") a["name"] = "Включить питание";
    const auto call=[&](const Json& request){return Json::parse(worker.handle(request.dump()));};
    auto initialized=call({{"method","apply"},{"configuration",config}});
    if(!initialized.value("ok",false))throw std::runtime_error(initialized.dump());
    const auto entered=call({{"method","setup_enter"}});
    if(!entered.value("ok",false))throw std::runtime_error(entered.dump());
    Json report=Json::array();
    for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"location","place_a"},{"device","lamp_a"},{"action","power_on"}}){
        auto reply=call({{"method","language_profile"},{"configuration",config},{"entity",pair.first},{"id",pair.second}});
        reply.erase("active");
        report.push_back({{"entity",pair.first},{"id",pair.second},{"reply",reply}});
        std::ofstream(argv[3])<<report.dump(2)<<'\n';
        std::cout<<pair.first<<" ok="<<reply.value("ok",false)<<std::endl;
    }
    for(const auto* method:{"setup_restore","setup_finish"}){const auto reply=call({{"method",method}});if(!reply.value("ok",false))throw std::runtime_error(reply.dump());}
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
