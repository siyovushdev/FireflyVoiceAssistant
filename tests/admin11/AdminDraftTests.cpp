#include "../admin/Support.h"
#include "firefly/admin/AdminDraft.h"
#include <future>
#include <iostream>
#include <chrono>
using namespace admin_test;
using namespace firefly::admin;
namespace {
int checks=0;
void check(bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F fn,const std::string& message){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed,message);}
Json factory(Json c){
    c["rooms"].push_back({{"id","location_factory"},{"name","Factory"},{"speech",{{"mentions",Json::array()},{"locations",Json::array()}}},{"metadata",{{"type","factory"},{"container",true}}}});
    c["rooms"].push_back({{"id","location_packaging"},{"name","Цех упаковки"},{"speech",{{"mentions",{"упаковки"}},{"locations",{"в цехе упаковки"}}}},{"metadata",{{"type","workshop"},{"parent_id","location_factory"}}}});
    c["devices"].push_back({{"id","device_packaging_light"},{"name","Освещение упаковки"},{"kind","light"},{"room_id","location_packaging"},{"enabled",true},{"action_ids",{"action_set_brightness"}},{"spoken_aliases",Json::array()},{"metadata",{{"type","light"},{"aliases",{"свет упаковки","освещение упаковки"}}}}});
    return c;
}
}
int main(int argc,char** argv){try{
    if(argc!=2)return 2;const auto b=base(argv[1]);
    check(deterministicId("location","Ванная")=="location_vannaya","Russian transliteration");
    check(deterministicId("location","Цех упаковки")=="location_tsekh_upakovki","workshop deterministic");
    check(deterministicId("device","Конвейер подачи №2")=="device_konveyer_podachi_2","numeric punctuation");
    check(deterministicId("device"," A-- B  ")=="device_a_b","collapse separators");
    check(deterministicId("device","Ёж")=="device_yozh","yo");
    check(deterministicId("device","x",{"device_x","device_x_2"})=="device_x_3","collision stable suffix");
    check(deterministicId("device",std::string(100,'a')).size()==64,"bounded ID");
    rejects([]{deterministicId("policy","x");},"unknown category");
    rejects([]{deterministicId("device",std::string("\xff"));},"invalid UTF8");
    const std::string valid=R"({"type":"workshop","aliases":["упаковочный цех"],"mentions":["упаковки"],"locations":["в цехе упаковки"],"capabilities":[]})";
    check(Json::parse(decodeSuggestion(valid))["type"]=="workshop","strict useful suggestion");
    for(const auto& raw:std::vector<std::string>{"{}","null","[]",valid+"x","```json\n"+valid+"\n```",R"({"type":"x","type":"y","aliases":[],"mentions":[],"locations":[],"capabilities":[]})"})rejects([&]{decodeSuggestion(raw);},"invalid structured response");
    for(const auto* field:{"type","aliases","mentions","locations","capabilities"}){auto v=Json::parse(valid);v.erase(field);rejects([&]{decodeSuggestion(v.dump());},"missing field");}
    auto v=Json::parse(valid);v["apply"]=true;rejects([&]{decodeSuggestion(v.dump());},"AI cannot apply");
    v=Json::parse(valid);v["mentions"]={"кабинет 1"};check(Json::parse(decodeSuggestion(v.dump()))["mentions"][0]=="кабинет 1","natural multi-token mentions accepted");
    v=Json::parse(valid);v["aliases"]={"a","a"};rejects([&]{decodeSuggestion(v.dump());},"duplicate suggestion");
    check(suggestionPrompt("location").find("kitchen")==std::string::npos,"universal AI prompt");
    auto f=factory(b);auto cat=catalog(f);check(cat->configuration().rooms.size()==6,"generic Locations");
    check(cat->configuration().rooms[0].metadata.parentId.has_value() || cat->roomIndex().count("location_packaging"),"hierarchy retained");
    check(catalog(cat->canonicalJson().empty()?b:Json::parse(cat->canonicalJson()))->contentHash()==cat->contentHash(),"extension roundtrip");
    auto bad=f;bad["rooms"][4]["metadata"]["parent_id"]="location_packaging";rejects([&]{catalog(bad);},"hierarchy cycle");
    bad=f;bad["rooms"][5]["metadata"]["parent_id"]="missing";rejects([&]{catalog(bad);},"unknown parent");
    bad=f;bad["rooms"][4]["metadata"]["disable_safety"]=true;rejects([&]{catalog(bad);},"metadata closed schema");
    bad=f;bad["rooms"][4]["speech"]["mentions"]={"factory"};rejects([&]{catalog(bad);},"container aliases cannot authorize commands");
    const auto diff=Json::parse(configurationDiff(b.dump(),f.dump()));check(diff.size()==3,"human diff additions");
    Model model;RuntimeWorker worker(model);LocalRuntimeClient client(worker);Directory dir;firefly::config::ConfigStore store(dir.path);store.initialize(catalog(b));Api api(store,client);api.initializeRuntime();
    auto call=[&](const std::string& method,const std::string& path,Json body=Json::object()){const auto r=api.handle(method,path,method=="GET"?"":body.dump());return std::pair<int,Json>{r.status,Json::parse(r.body)};};
    auto identity=[&](){const auto s=store.current();return Json{{"revision",s->configuration().revision},{"hash",s->contentHash()}};};
    const auto initial=store.current()->contentHash();const auto initialState=call("GET","/api/devices/state").second;
    model.raw=valid;
    auto reply=call("POST","/api/admin/suggest",{{"entity","location"},{"name","Цех упаковки"}});
    check(reply.first==200 && reply.second["suggestion"]["type"]=="workshop","AI draft endpoint");
    check(reply.second["saved"]==false && reply.second["applied"]==false,"proposal never saves/applies");
    check(store.current()->contentHash()==initial && call("GET","/api/devices/state").second==initialState,"suggestion leaves entire runtime/store unchanged");
    check(model.prompt==call("GET","/api/config/active").second["active"]["context"],"command context restored");
    model.raw="bad";reply=call("POST","/api/admin/suggest",{{"entity","device"},{"name","Конвейер"}});check(reply.first==422 && reply.second["errors"][0]["code"]=="invalid_ai_response","malformed AI fail closed");
    check(reply.second["raw_rkllm"]=="bad","invalid raw diagnostic retained");
    model.available=false;check(call("POST","/api/admin/suggest",{{"entity","location"},{"name","Цех"}}).second["errors"][0]["code"]=="ai_unavailable","AI unavailable");
    check(call("GET","/api/config").first==200,"manual management available");
    const auto offlineValidation=call("POST","/api/config/validate",{{"configuration",b}});
    check(offlineValidation.first==200 && offlineValidation.second["runtime_preflight"]=="unavailable","manual validation independent of AI availability");
    model.available=true;
    auto exported=call("GET","/api/config/export").second;check(exported["schema_version"]==1 && catalog(exported)->contentHash()==initial,"valid export");
    reply=call("POST","/api/config/import",{{"document",f.dump()}});check(reply.first==200 && reply.second["saved"]==false && store.current()->contentHash()==initial,"import as candidate");
    check(call("POST","/api/config/import",{{"document",R"({"schema_version":1,"schema_version":1})"}}).first==422,"import duplicate keys strict");
    check(call("POST","/api/config/validate",{{"configuration",f}}).first==200,"Factory validates");
    check(call("PUT","/api/config",{{"configuration",f},{"expected",identity()}}).first==200,"Factory saved");
    check(call("POST","/api/config/apply",{{"expected",identity()}}).first==200,"Factory applied");
    check(call("POST","/api/test-command",{{"text","Включи"},{"execute",true},{"matching_evidence",Json::array({{{"entity_id","device_packaging_light"},{"method","exact"}}})}}).first==400,"client cannot supply grounding authorization");
    const auto before=call("GET","/api/devices/state").second;
    const std::string command="В цехе упаковки поставь свет на 60 процентов";
    reply=call("POST","/api/test-command",{{"text",command},{"execute",false}});
    check(reply.second["result"]==light("device_packaging_light",60) && reply.second["execution"].is_null(),"Factory preview");
    check(call("GET","/api/devices/state").second==before && call("GET","/api/executions").second["executions"].empty(),"preview no state/history mutation");
    reply=call("POST","/api/test-command",{{"text",command},{"execute",true}});
    check(reply.second["execution"]["previous"]["brightness"]==0 && reply.second["execution"]["current"]["brightness"]==60,"Factory execute 0 to60");
    const auto after=call("GET","/api/devices/state").second;
    model.raw=light("device_packaging_light",0).dump();reply=call("POST","/api/test-command",{{"text","В цехе упаковки включи освещение на 0 процентов"},{"execute",true}});
    check(reply.second["result"]["unsupported"]==true && call("GET","/api/devices/state").second==after,"unsafe never executes");
    const auto revisions=call("GET","/api/config/revisions").second;check(revisions["revisions"].size()==1 && revisions["revisions"][0]["timestamp_ms"]>0,"revision history/time");
    const auto hash=store.current()->contentHash();reply=call("POST","/api/config/restore-candidate",{{"revision",1},{"expected",identity()}});
    check(reply.first==200 && reply.second["applied"]==false && store.current()->contentHash()==hash,"restore only candidate");
    check(call("GET","/api/devices/state").second==after,"restore leaves active states alone");
    auto renamed=Json::parse(store.current()->canonicalJson());for(auto& r:renamed["rooms"])if(r["id"]=="location_packaging")r["name"]="Участок упаковки";
    check(call("PUT","/api/config",{{"configuration",renamed},{"expected",identity()}}).first==200,"rename saved");
    check(store.current()->roomIndex().count("location_packaging"),"rename stable identity");
    auto bath=bathroom(b);check(call("POST","/api/config/validate",{{"configuration",bath}}).first==200,"Bathroom regression validates");
    worker.handle(Json{{"method","apply"},{"configuration",bath}}.dump());
    reply=call("POST","/api/test-command",{{"text","В ванной поставь свет на 40 процентов"},{"execute",true}});
    check(reply.second["result"]==light("device_bathroom_light",40) && reply.second["execution"]["success"]==true,"Bathroom regression executes");
    const auto dash=call("GET","/api/dashboard").second;check(!dash["simple"].contains("active") && dash["advanced"].contains("active"),"Simple/Advanced projections");
    auto inventory=b;inventory["devices"].push_back({{"id","device_conveyor"},{"name","Конвейер"},{"kind","generic"},{"room_id","room_kitchen"},{"enabled",true},{"action_ids",Json::array()},{"spoken_aliases",Json::array()},{"metadata",{{"type","conveyor"}}}});
    RuntimeGeneration generic(catalog(inventory));check(generic.executor->states().size()==6,"generic inventory graceful mock state");
    inventory["devices"].back()["action_ids"]={"action_set_brightness"};rejects([&]{catalog(inventory);},"generic cannot gain executable capability");
    Directory offlineDirectory;firefly::config::ConfigStore offlineStore(offlineDirectory.path);offlineStore.initialize(catalog(b));
    Model offlineModel;offlineModel.available=false;RuntimeWorker offlineWorker(offlineModel);LocalRuntimeClient offlineClient(offlineWorker);Api offlineApi(offlineStore,offlineClient);offlineApi.initializeRuntime();
    const auto offlineCall=[&](const std::string& method,const std::string& path,Json body){const auto r=offlineApi.handle(method,path,body.dump());return std::pair<int,Json>{r.status,Json::parse(r.body)};};
    check(offlineCall("POST","/api/config/validate",{{"configuration",b}}).first==200,"offline manual Validate");
    auto offlineIdentity=Json{{"revision",1},{"hash",offlineStore.current()->contentHash()}};
    const auto offlineSaved=offlineCall("PUT","/api/config",{{"configuration",b},{"expected",offlineIdentity}});
    check(offlineSaved.first==200,"offline manual Save");
    const auto firstDiff=offlineCall("POST","/api/config/diff",{{"configuration",b},{"base","active"}});
    check(firstDiff.first==200 && firstDiff.second["changes"].size()>=11,"initial Apply diff does not require available AI");
    offlineIdentity={{"revision",offlineSaved.second["revision"]},{"hash",offlineSaved.second["hash"]}};
    offlineModel.available=true;
    check(offlineCall("POST","/api/config/apply",{{"expected",offlineIdentity}}).first==200,"manual Apply available after model recovery");
    // A suggestion holds the same lease as commands/Apply and restores before release.
    Model blocked;RuntimeGenerations generations(blocked);generations.apply(catalog(b));blocked.raw=valid;blocked.block=true;
    auto suggestion=std::async(std::launch::async,[&]{return generations.suggest("location","Цех упаковки");});
    {std::unique_lock<std::mutex> lock(blocked.mutex);blocked.cv.wait(lock,[&]{return blocked.entered;});}
    auto apply=std::async(std::launch::async,[&]{generations.apply(catalog(f));});
    check(apply.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"Apply waits for admin inference");
    check(generations.current()->id==1,"active retained during AI");
    {std::lock_guard<std::mutex> lock(blocked.mutex);blocked.release=true;blocked.cv.notify_all();}suggestion.get();apply.get();
    check(generations.current()->id==2 && blocked.prompt==generations.current()->prompt,"Apply publishes after AI restoration");
    check(generations.process(command,true).execution->current==firefly::execution::DeviceState{firefly::execution::LightState{60}},"next request new domain");
    Model leased;RuntimeGenerations leaseRuntime(leased);leaseRuntime.apply(catalog(b));leased.raw=valid;
    auto held=leaseRuntime.beginRequest();
    auto waiting=std::async(std::launch::async,[&]{return leaseRuntime.suggest("location","Цех");});
    check(waiting.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"AI waits for existing voice/request lease");
    check(leased.inferences==0,"no concurrent rkllm run while request admitted");
    held->process("На кухне поставь свет на 40 процентов",true,"voice");held.reset();waiting.get();
    check(leaseRuntime.history().size()==1 && leaseRuntime.history()[0].source=="voice","AI does not corrupt or execute voice event");
    class FailingModel : public Model { public: std::string infer(std::string_view) override { throw std::runtime_error("inference failure"); } } failing;
    RuntimeGenerations failureRuntime(failing);failureRuntime.apply(catalog(b));
    rejects([&]{failureRuntime.suggest("device","Насос");},"AI inference failure reported");
    check(failing.prompt==failureRuntime.current()->prompt,"failure restores command context");
    check(failureRuntime.process("На кухне поставь свет на 40 процентов",true).execution->success,"deterministic command after AI failure");
    std::cout<<"PASS "<<checks<<" generalized admin checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
