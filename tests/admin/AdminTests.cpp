#include "Support.h"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
using namespace admin_test;
using namespace firefly;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    int checks=0,failures=0;
    const auto check=[&](bool pass,const std::string& name){++checks;if(!pass){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    const auto a=base(argv[1]), b=bathroom(a);
    const auto ca=catalog(a),cb=catalog(b);
    Model model;
    firefly::admin::RuntimeGenerations runtime(model);
    check(!runtime.current(),"no half initialized generation");
    runtime.apply(ca);
    const auto old=runtime.current();
    check(old->domain.snapshot()==ca && model.prompt==old->prompt,"startup owns exact snapshot and context");
    check(typed(runtime.process("На кухне поставь свет на 40 процентов").command.action)==light("device_kitchen_light",40),"actual parser current M4");
    runtime.preflight(cb);
    check(runtime.current()==old && model.prompt==old->prompt,"validate leaves active generation/context unchanged");
    model.reject=true;
    try {runtime.apply(cb);check(false,"failed apply throws");}catch(...){check(true,"failed apply throws");}
    check(runtime.current()==old && model.prompt==old->prompt,"failed apply retains old");model.reject=false;
    runtime.apply(cb);
    check(runtime.current()!=old && old->domain.snapshot()==ca,"old snapshot retained by reader");
    check(runtime.current()->prompt.find("device_bathroom_light")!=std::string::npos && old->prompt.find("device_bathroom_light")==std::string::npos,"context changes with snapshot");
    for (const auto& phrase:{"В ванной поставь свет на 40 процентов","В ванне поставь свет на 40 процентов"}) {
        const auto result=runtime.process(phrase);
        check(result.command.path==firefly::CommandPath::FastPath && typed(result.command.action)==light("device_bathroom_light",40),"Bathroom explicit alias production parser");
    }
    model.raw=light("device_bathroom_light",30).dump();
    auto result=runtime.process("Пожалуйста, в ванной свет 30 процентов");
    check(result.command.path==firefly::CommandPath::LlmFallback && typed(result.command.action)==light("device_bathroom_light",30),"new target LLM typed ingress");
    model.raw=light("device_kitchen_light",50).dump();
    check(typed(runtime.process("На кухне выключи свет на 50 процентов").command.action)==Json{{"unsupported",true}},"contradiction02 rejects injected 50");
    model.raw=light("device_kitchen_light",0).dump();
    check(typed(runtime.process("На кухне выключи свет на 50 процентов").command.action)==Json{{"unsupported",true}},"contradiction02 cannot repair to zero");
    Json narrow=b; narrow["actions"][0]["parameters"][0]["minimum"]=10; narrow["actions"][0]["parameters"][0]["maximum"]=90;
    // M10 adds retained mock state. Arrange a value valid in both domains;
    // all original M9 understanding/range expectations below remain unchanged.
    for (const auto& location : {"На кухне", "В спальне", "В гостиной", "В коридоре", "В ванной"})
        runtime.process(std::string(location) + " поставь свет на 40 процентов", execution::ExecutionIntent::Playground);
    runtime.apply(catalog(narrow));
    for(const auto& room:std::vector<std::pair<std::string,std::string>>{{"На кухне","device_kitchen_light"},{"В спальне","device_bedroom_light"},{"В гостиной","device_living_light"},{"В коридоре","device_corridor_light"},{"В ванной","device_bathroom_light"}})
        for(int value:{0,9,10,40,90,91,100}) {
            const auto command=runtime.process(room.first+" поставь свет на "+std::to_string(value)+" процентов");
            check(typed(command.command.action)==(value>=10&&value<=90?light(room.second,value):Json{{"unsupported",true}}),"range from new generation all rooms");
        }
    Json capability=b;
    auto separate=capability["actions"][0];separate["id"]="action_bathroom_brightness";
    separate["parameters"][0]["minimum"]=20;separate["parameters"][0]["maximum"]=80;
    capability["actions"].push_back(separate);capability["devices"][5]["action_ids"]={"action_bathroom_brightness"};
    runtime.apply(catalog(capability));
    auto specific=light("device_bathroom_light",40);specific["actionId"]="action_bathroom_brightness";
    check(typed(runtime.process("В ванной поставь свет на 40 процентов").command.action)==specific,"new registered capability stable ID from config");
    check(typed(runtime.process("В ванной поставь свет на 90 процентов").command.action)==Json{{"unsupported",true}},"device-specific action range used");
    check(typed(runtime.process("На кухне поставь свет на 90 процентов").command.action)==light("device_kitchen_light",90),"separate action leaves original range unchanged");
    Json door=b;for(auto& device:door["devices"])if(device["kind"]=="entrance_door")device["name"]="Configured entrance";
    runtime.apply(catalog(door));
    check(runtime.current()->domain.entranceTarget()->name=="Configured entrance" && runtime.current()->prompt.find("Configured entrance")!=std::string::npos,"entrance metadata and LLM same generation");
    model.raw=R"({"actionId":"action_open_entrance","targetId":"device_entrance_door","arguments":{}})";
    for(const auto& entry:std::vector<std::pair<std::string,bool>>{{"дверь",false},{"открой дверь",false},{"открой входную дверь",true},{"открыть дверь на входе",true},{"открой балконную дверь",false}})
        check(!typed(runtime.process(entry.first).command.action).contains("unsupported")==entry.second,"door deterministic safety "+entry.first);
    // In-flight request holds old generation until completion; Apply serializes.
    runtime.apply(ca);model.block=true;model.entered=false;model.release=false;
    const auto before=runtime.current();
    auto processing=std::async(std::launch::async,[&]{return runtime.process("Открой входную дверь");});
    {std::unique_lock<std::mutex> lock(model.mutex);model.cv.wait(lock,[&]{return model.entered;});}
    auto applying=std::async(std::launch::async,[&]{runtime.apply(cb);});
    check(applying.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout && runtime.current()==before,"apply waits for old inference");
    {std::lock_guard<std::mutex> lock(model.mutex);model.release=true;}model.cv.notify_all();
    check(processing.get().generation==before,"inflight result identity old");applying.get();model.block=false;
    check(runtime.current()->domain.snapshot()==cb,"atomic publication after old inference");

    Directory dir; firefly::config::ConfigStore store(dir.path);const auto seeded=store.initialize(ca);
    Model apiModel;Client client(apiModel);firefly::admin::Api api(store,client);api.initializeRuntime();
    auto call=[&](const std::string& method,const std::string& path,const Json& body=Json()) {
        const auto reply=api.handle(method,path,body.is_null()?"":body.dump());return std::make_pair(reply.status,Json::parse(reply.body));
    };
    const auto identity=[](const firefly::config::Catalog& c){return Json{{"revision",c->configuration().revision},{"hash",c->contentHash()}};};
    auto status=call("GET","/api/status");check(status.first==200 && status.second["runtime_ready"] && !status.second["needs_apply"],"API status initial");
    const auto original=call("GET","/api/config").second;
    check(original["hash"]==seeded->contentHash(),"API reads existing store");
    auto validated=call("POST","/api/config/validate",{{"configuration",b}});
    check(validated.first==200 && validated.second["apply_ready"] && store.current()->contentHash()==seeded->contentHash(),"Validate no save and preflight exact seam");
    check(call("GET","/api/config/active").second["active"]["hash"]==seeded->contentHash(),"Validate no apply");
    auto saved=call("PUT","/api/config",{{"configuration",b},{"expected",identity(seeded)}});
    check(saved.first==200 && saved.second["revision"]==2 && !saved.second["applied"],"Save assigns revision");
    check(call("GET","/api/status").second["needs_apply"],"stored differs active");
    check(call("PUT","/api/config",{{"configuration",a},{"expected",identity(seeded)}}).first==409,"stale concurrent save rejected");
    check(call("POST","/api/config/apply",{{"expected",identity(seeded)}}).first==409,"stale apply rejected");
    const auto stored=store.current();apiModel.reject=true;
    check(call("POST","/api/config/apply",{{"expected",identity(stored)}}).first==422,"failed Apply explicit");apiModel.reject=false;
    check(call("GET","/api/config/active").second["active"]["hash"]==seeded->contentHash(),"failed Apply old active");
    check(call("POST","/api/test-command",{{"text","На кухне поставь свет на 40 процентов"}}).second["result"]==light("device_kitchen_light",40),"old active services commands after failure");
    check(call("POST","/api/config/apply",{{"expected",identity(stored)}}).first==200,"Apply stored");
    const auto active=call("GET","/api/config/active").second["active"];
    check(active["hash"]==stored->contentHash() && active["context"].get<std::string>().find("device_bathroom_light")!=std::string::npos,"applied context proof");
    for(const auto& [text,value]:std::vector<std::pair<std::string,int>>{{"В ванной поставь свет на 40 процентов",40},{"В ванне поставь свет на 30 процентов",30}}) {
        auto command=call("POST","/api/test-command",{{"text",text}});
        check(command.first==200 && command.second["route"]=="deterministic" && command.second["result"]==light("device_bathroom_light",value),"API Bathroom end-to-end "+text);
    }
    std::vector<std::pair<std::string,Json>> invalid;
    auto bad=b;bad["rooms"].push_back(bad["rooms"][0]);invalid.push_back({"duplicate ID",bad});
    bad=b;bad["rooms"][4]["speech"]["mentions"].push_back("кухне");invalid.push_back({"alias collision",bad});
    bad=b;bad["devices"][5]["room_id"]="room_missing";invalid.push_back({"broken reference",bad});
    bad=b;bad["devices"].erase(5);check(call("POST","/api/config/validate",{{"configuration",bad}}).first==200,"Location without executable device is valid");
    bad=b;bad["devices"].erase(4);invalid.push_back({"missing entrance",bad});
    bad=b;bad["actions"][0]["parameters"][0]["minimum"]=101;invalid.push_back({"invalid range",bad});
    bad=b;bad["actions"][0]["operation"]="unsafe.anything";invalid.push_back({"malformed capability",bad});
    bad=b;bad["schema_version"]=2;invalid.push_back({"unknown schema",bad});
    bad=b;bad["devices"][5]["enabled"]=false;invalid.push_back({"unimplemented editable policy rejected",bad});
    bad=b;auto extraDoor=b["devices"][4];extraDoor["id"]="device_other_door";bad["devices"].push_back(extraDoor);invalid.push_back({"additional unsupported entrance target",bad});
    bad=b;auto disabledDefinition=b["actions"][0];disabledDefinition["id"]="action_disabled";disabledDefinition["enabled"]=false;bad["actions"].push_back(disabledDefinition);invalid.push_back({"disabled unbound capability",bad});
    for(const auto& [name,configuration]:invalid) {
        check(call("POST","/api/config/validate",{{"configuration",configuration}}).first==422,"validate rejects "+name);
        check(call("PUT","/api/config",{{"configuration",configuration},{"expected",identity(stored)}}).first==422,"save rejects "+name);
        check(store.current()->contentHash()==stored->contentHash() && call("GET","/api/config/active").second["active"]["hash"]==stored->contentHash(),"invalid config cannot mutate "+name);
    }
    Json large=b;
    for(int i=0;i<8;++i) {
        const std::string suffix(1,static_cast<char>('a'+i));
        auto room=Json{{"id","room_extra_"+suffix},{"name","Extra"},{"speech",{{"mentions",Json::array({"extra"+suffix})},{"locations",Json::array({"в extra"+suffix})}}}};
        for(int j=0;j<26;++j)room["speech"]["mentions"].push_back("alias"+suffix+std::string(1,static_cast<char>('a'+j))+std::string(75,'z'));
        large["rooms"].push_back(room);
        auto device=b["devices"][5];device["id"]="device_extra_"+suffix;device["room_id"]="room_extra_"+suffix;large["devices"].push_back(device);
    }
    check(catalog(large)!=nullptr,"oversized context fixture is schema-valid");
    check(call("POST","/api/config/validate",{{"configuration",large}}).first==422,"oversized context rejected offline byte bound");
    check(call("PUT","/api/config",{{"configuration",large},{"expected",identity(stored)}}).first==422,"byte-oversized prompt not saved");
    bad=b;bad["devices"][5]["name"]="REJECT_CONTEXT";
    check(call("POST","/api/config/validate",{{"configuration",bad}}).first==422,"runtime preflight rejection");
    check(apiModel.prompt==active["context"],"failed preflight preserves context");
    check(call("GET","/api/config/revisions").second["revisions"].size()==1,"backup retained");
    check(call("POST","/api/config/restore",{{"revision",1},{"expected",identity(stored)}}).first==200,"restore creates new stored revision");
    check(call("GET","/api/status").second["needs_apply"],"restore no implicit Apply");
    check(call("POST","/api/config/reconcile",Json::object()).first==200,"explicit reconcile");
    // Runtime busy and outage do not block configuration management.
    apiModel.block=true;apiModel.entered=false;apiModel.release=false;
    auto blocked=std::async(std::launch::async,[&]{return call("POST","/api/test-command",{{"text","Открой входную дверь"}});});
    {std::unique_lock<std::mutex> lock(apiModel.mutex);apiModel.cv.wait(lock,[&]{return apiModel.entered;});}
    check(call("GET","/api/status").second["runtime_busy"] && call("GET","/api/config").first==200,"editor available during inference");
    check(call("POST","/api/test-command",{{"text","x"}}).first==409,"concurrent runtime command explicit busy");
    {std::lock_guard<std::mutex> lock(apiModel.mutex);apiModel.release=true;}apiModel.cv.notify_all();blocked.get();
    apiModel.available=false;
    check(call("GET","/api/config").first==200,"editor while model unavailable");
    for(const auto& entry:std::vector<std::pair<std::string,int>>{{"{",400},{"{}",400},{"[]",400},{"{\"text\":\"a\",\"text\":\"b\"}",400},{"{\"text\":42}",400},{"{\"text\":\"x\",\"extra\":1}",400}})
        check(api.handle("POST","/api/test-command",entry.first).status==entry.second,"malformed API message");
    check(call("GET","/api/unknown").first==404 && call("DELETE","/api/config",Json::object()).first==405,"paths and methods");
    for(const auto step:{firefly::config::StoreStep::Write,firefly::config::StoreStep::FileSync,firefly::config::StoreStep::Rename,firefly::config::StoreStep::DirectorySync}) {
        Directory faultDir;firefly::config::ConfigStore clean(faultDir.path);const auto initial=clean.initialize(ca);
        bool inject=true;firefly::config::ConfigStore broken(faultDir.path,[&](auto current){if(inject && current==step)throw std::runtime_error("injected save fault");});
        Model faultModel;Client faultClient(faultModel);firefly::admin::Api faultApi(broken,faultClient);faultApi.initializeRuntime();
        const auto response=faultApi.handle("PUT","/api/config",Json{{"configuration",b},{"expected",identity(initial)}}.dump());
        check(response.status!=200,"failed save reported");inject=false;
        check(Json::parse(faultApi.handle("GET","/api/config/active","").body)["active"]["hash"]==initial->contentHash(),"failed save never changes active");
        check(faultApi.handle("POST","/api/config/reconcile","{}").status==200,"fault recovery explicit");
        const auto recovered=clean.current();check(recovered->configuration().revision==1 || recovered->configuration().revision==2,"recovery always complete valid snapshot");
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
