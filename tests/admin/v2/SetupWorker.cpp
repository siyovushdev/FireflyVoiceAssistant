// Test-only scripted model outputs. Real SetupSession, strict decoders and API.
#include "../Support.h"
#include "Ipc.h"
#include <poll.h>
#include <iostream>
#include <thread>
using admin_test::Json;
class Model final : public admin_test::Model {
public:
    Json request;
    bool setupRequired() const override { return true; }
    void enterSetup() override {
        const char* gate=std::getenv("FIREFLY_B3_ENTRY_GATE");
        if(!gate)return;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        while(::access(gate,F_OK)==0) {
            if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("fixture entry gate timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    void leaveSetup(const std::string& p) override { useContext(p); }
    void useAdminContext(const std::string& p, int) override { prompt=p; }
    std::string inferAdmin(std::string_view input) override {
        const std::string text(input);
        if(text.find("сломанный")!=std::string::npos)return "{broken";
        if(prompt.find("Ровно две строки JSON")!=std::string::npos)return R"({"description":"Производственная зона или оборудование","type":"equipment"})";
        if(prompt.find("Ровно поля template")!=std::string::npos)return R"({"template":"custom","capabilities":[]})";
        if(prompt.find("Верни шесть строк JSON")!=std::string::npos) {
            const auto name=request.value("name",std::string("цех"));
            const auto location=name=="Цех"?std::string("в цехе"):"в "+name;
            return Json{{"name",name},{"short",name},{"synonym",name},{"name_location",location},{"short_location",location},{"synonym_location",location}}.dump();
        }
        if(prompt.find("Предложи только небольшой русский")!=std::string::npos) {
            const auto category=request.value("entity",std::string()); const auto id=request.value("id",std::string());
            if(category=="location") {
                for(const auto& zone:request.at("configuration").at("rooms"))if(zone.at("id")==id) {
                    const auto name=zone.at("name").get<std::string>();
                    const std::map<std::string,std::string> locations={{"Детская","в детской"},{"Цех упаковки","в цехе упаковки"},{"Склад","на складе"},{"Лабораторный стенд","на лабораторном стенде"}};
                    const auto location=locations.count(name)?locations.at(name):"в "+name;
                    return Json{{"aliases",Json::array({name})},{"mentions",Json::array({name})},{"locations",Json::array({location})}}.dump();
                }
            }
            if(category=="action")return prompt.find("false")!=std::string::npos?R"({"aliases":["выключи","выключить"]})":R"({"aliases":["включи","включить"]})";
            return Json{{"aliases",Json::array({"оборудование "+id})}}.dump();
        }
        if(request.value("method",std::string())=="copilot") {
            const auto& c=request.at("configuration");
            const auto start=text.find("Явные факты пользователя: ")+std::string("Явные факты пользователя: ").size();
            const auto end=text.find(". Не меняй семантику.",start);const auto f=Json::parse(text.substr(start,end-start));
            const auto left=text.find('('),right=text.find(')',left);const auto target=text.substr(left+1,right-left-1);
            Json connection={{"id","connection_test"},{"name","HTTP Test"},{"type","http"},{"enabled",true},{"timeout_ms",2000}};
            connection["http"]={{"base_url",f.at("endpoint")},{"base_path",""}};
            Json ops=Json::array();ops.push_back({{"op","set_http_connection"},{"connection",connection}});
            const Json* device=nullptr;for(const auto& d:c.at("devices"))if(d.at("id")==target)device=&d;
            if(!device)throw std::runtime_error("fixture target missing");
            for(const auto& a:c.at("actions"))if(std::find(device->at("action_ids").begin(),device->at("action_ids").end(),a.at("id"))!=device->at("action_ids").end()) {
                const auto& fact=f.at(a.at("assignments")[0].at("value").get<bool>()?"on":"off");
                Json binding={{"target_id",target},{"action_id",a.at("id")},{"connection_id","connection_test"},{"executor","http"}};
                binding["http"]={{"method",fact.at("method")},{"path",fact.at("path")},{"body",fact.at("body")},{"expected_status",200}};
                ops.push_back({{"op","set_connection_binding"},{"binding",binding}});
            }
            if(!f.at("noObserve").get<bool>()) {
                Json observation={{"target_id",target},{"connection_id","connection_test"},{"executor","http"}};
                observation["http"]={{"method","GET"},{"path",f.at("observe")},{"expected_status",200}};
                observation["fields"]={{f.at("field").get<std::string>(),"power"}};
                ops.push_back({{"op","set_http_observe"},{"observation",observation}});
            }
            return Json{{"status","ok"},{"summary","HTTP contract из явных фактов"},{"operations",ops}}.dump();
        }
        if(request.value("method",std::string())=="action_draft")return Json{{"description","Питание устройства"},{"aliases",Json::array({"включи"})},{"assignments",Json::array({{{"field","power"},{"value",true}}})}}.dump();
        if(prompt.find("Извлеки НОВОЕ имя")!=std::string::npos)return R"({"name":"Вытяжка"})";
        if(text.find("без зоны")!=std::string::npos)return R"({"ops":[["power_device",null,"Контроллер стенда"]]})";
        if(text.find("Детскую")!=std::string::npos)return R"({"ops":[["location","Детская"],["power_device","Детская","Свет"],["power_device","Детская","Телевизор"]]})";
        if(text.find("вентилятор")!=std::string::npos)return R"({"ops":[["power_device","Цех упаковки","Вентилятор"]]})";
        for(const auto& zone:{"Цех упаковки","Склад","Лабораторный стенд"})if(text.find(zone)!=std::string::npos)return Json{{"ops",Json::array({Json::array({"location",zone})})}}.dump();
        return R"({"question":"Уточните устройство и необходимые возможности."})";
    }
};
int main() {
    Model model;firefly::admin::RuntimeWorker worker(model);
    try { while(true) {
        pollfd descriptor{3,POLLIN,0};if(::poll(&descriptor,1,-1)<=0 || descriptor.revents&(POLLHUP|POLLERR))break;
        const auto bytes=firefly::admin::receiveMessage(3,5);model.request=Json::parse(bytes);
        const auto reply=worker.handle(bytes);
        if(!Json::parse(reply).value("ok",false))std::cerr<<reply<<'\n';
        firefly::admin::sendMessage(3,reply,5);
    }}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
}
