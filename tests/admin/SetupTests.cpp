#include "Support.h"
#include "SetupSession.h"
#include "DialogueOperations.h"
#include "firefly/InputPolicy.h"
#include "firefly/admin/Api.h"
#include <iostream>
#include <sys/wait.h>
using namespace firefly::admin;
using admin_test::Json;
struct SetupModel : admin_test::Model {
    bool admin=false;std::vector<std::string> events;
    bool setupRequired() const override { return true; }
    void enterSetup() override { events.push_back("runtime_destroy");admin=true;events.push_back("admin_load"); }
    void leaveSetup(const std::string& text) override { events.push_back("admin_destroy");admin=false;useContext(text);events.push_back("runtime_load"); }
    void useAdminContext(const std::string& p,int) override { if(!admin)throw std::runtime_error("setup_required");prompt=p; }
    std::string inferAdmin(std::string_view text) override {
        validateMessage(text);
        const auto source=raw;
        // Scripted per-step model replies. No real language understanding is mocked
        // in the configuration/decoder/lifecycle, only the vendor output seam.
        try {
            auto wire=Json::parse(raw);
            if(wire.contains("ops") && wire["ops"].size()>1) {
                if(prompt.find("Извлеки НОВОЕ имя")!=std::string::npos) {
                    for(const auto& op:wire["ops"])if(op[0]=="rename_device")raw=Json{{"name",op[3]}}.dump();
                } else if(prompt.find("Помощник настройки дома")!=std::string::npos) {
                    Json selected=Json::array();
                    for(const auto& op:wire["ops"]){const auto kind=op[0].get<std::string>();
                        if((std::string(text).find("Создай")!=std::string::npos && kind=="location") ||
                           (std::string(text).find("Там")!=std::string::npos && kind=="power_device") ||
                           (std::string(text).find("Ещё")!=std::string::npos && kind=="power_device"))selected.push_back(op);}
                    if(!selected.empty())raw=Json{{"ops",selected}}.dump();
                }
            }
        }catch(...){}
        const auto result=infer(text);raw=source;return result;
    }
};
int main(int argc,char**argv){int failures=0,checks=0;
 auto check=[&](bool good,const char* name){++checks;if(!good){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
 auto rejected=[&](auto f){try{f();return false;}catch(...){return true;}};
 try{
 check(argc==2,"root argument");
 const auto operations=expandDialogueOperations(R"({"ops":[["location","Кабинет"],["power_device","Кабинет","Лампа"]]})");
 check(operations.at("operations")[1].at("template")=="power_only","short dialogue operations expand trusted capability fields");
 check(rejected([&]{expandDialogueOperations(R"({"ops":[["power_device","Кабинет","Лампа","generic_light"]]})");}),"extra operation arguments rejected");
 check(rejected([&]{expandDialogueOperations(R"({"ops":[["set_http_binding","1.2.3.4"]]})");}),"wire codec cannot create bindings");
 check(rejected([&]{expandDialogueOperations(R"({"ops":[["bounds","Зона","Лампа","level","0",100]]})");}),"numeric type never repaired");
 check(rejected([&]{expandDialogueOperations(R"({"ops":[],"question":"Какое?"})");}),"mixed wire variants rejected");
 check(rejected([&]{expandDialogueOperations("{\"ops\":[");}),"truncated operation result rejected");

 {
  SetupModel lifecycle;RuntimeGenerations generations(lifecycle);
  generations.apply(admin_test::catalog(admin_test::base(argv[1])));
  auto lease=generations.beginRequest();std::atomic<bool> drained{false},entered{false};
  generations.setAudioDrain([&]{drained=true;});
  std::thread entering([&]{try{generations.enterSetup();entered=true;}catch(...) {}});
  for(int i=0;i<1000&&!drained;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(drained&&generations.setupBlocked(),"setup closes admission before waiting for existing lease");
  check(rejected([&]{generations.beginRequest();}),"new requests rejected without queuing");
  check(lifecycle.events.empty(),"model not destroyed while accepted command holds lease");
  const auto result=lease->process("Включи свет на кухне",firefly::execution::ExecutionIntent::None,"test");
  check(std::holds_alternative<firefly::actions::GenericAction>(result.command.action),"accepted command finishes during entering setup");
  lease.reset();entering.join();check(entered&&lifecycle.admin,"admin loads only after lease completes");
  check(!generations.modelReady(),"no domestic READY while setup active");
  generations.prepareRestore();check(generations.setupBlocked(),"restoration keeps admission closed until finish");
  generations.finishRestore();check(generations.modelReady()&&!generations.setupBlocked(),"explicit finish reopens warmed runtime");
  generations.stop();check(rejected([&]{generations.beginRequest();}),"shutdown rejects new leases");
 }

 // API serialization must not delay the voice admission barrier until an
 // already accepted request has returned. No second inference is permitted.
 {
  struct BlockingModel : SetupModel {
   std::atomic<bool> pause{false},entered{false},release{false};
   void useContext(const std::string& prompt) override {
    if(pause){entered=true;while(!release)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    SetupModel::useContext(prompt);
   }
  } blocking;
  admin_test::Directory isolated;firefly::config::ConfigStore isolatedStore(isolated.path);
  const auto graph=isolatedStore.initialize(admin_test::catalog(admin_test::base(argv[1])));
  RuntimeWorker isolatedWorker(blocking);LocalRuntimeClient client(isolatedWorker);Api api(isolatedStore,client);
  api.initializeRuntime();blocking.pause=true;
  std::thread accepted([&]{api.handle("POST","/api/config/validate",Json{{"configuration",Json::parse(graph->canonicalJson())}}.dump());});
  for(int i=0;i<1000&&!blocking.entered;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(blocking.entered,"accepted API operation holds model before setup");
  Json enteredSetup;
  std::thread setup([&]{enteredSetup=Json::parse(api.handle("POST","/api/setup/start","{}").body);});
  for(int i=0;i<1000&&!isolatedWorker.setupBlocked();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool blockedBeforeRelease=isolatedWorker.setupBlocked();
  blocking.release=true;accepted.join();setup.join();
  check(blockedBeforeRelease,"local API closes voice admission before waiting for in-flight API operation");
  check(enteredSetup.value("ok",false),"setup waits then enters after accepted operation");
  const auto callsBeforeAmbiguity=blocking.inferences;
  const auto uncertain=Json::parse(isolatedWorker.handle(Json{{"method","dialogue"},{"configuration",Json::parse(graph->canonicalJson())},{"text","Измени диапазон яркости на кухне или в спальне."},{"focus",Json::array()}}.dump()));
  check(uncertain.value("ok",false) && uncertain.at("suggestion").at("status")=="ambiguous" && blocking.inferences==callsBeforeAmbiguity,"ambiguous capability or bounds edit asks instead of letting Qwen select a target");
  api.handle("POST","/api/setup/cancel",Json{{"session_id",enteredSetup.at("session_id")}}.dump());
 }

 std::string emoji;for(int i=0;i<4000;++i)emoji+="😀";
 check(validateMessage(emoji)==4000,"4000 unicode accepted");
 check(rejected([&]{validateMessage(emoji+"я");}),"4001 rejected");
 check(rejected([&]{validateMessage(std::string("\xc0\xaf",2));}),"invalid UTF8 rejected");
 check(rejected([&]{validateMessage("\x01");}),"control rejected");
 check(validateMessage(std::string(193,'x'))==193&&!firefly::validInput(std::string(193,'x')),"domestic cap unchanged");
 admin_test::Directory directory;firefly::config::ConfigStore store(directory.path);
 const auto base=store.initialize(firefly::config::ConfigLoader::fromFile(std::string(argv[1])+"/config/empty-home.json"));
 SetupModel model;RuntimeWorker worker(model);
 auto call=[&](const Json&r){return Json::parse(worker.handle(r.dump()));};
 check(call({{"method","apply"},{"configuration",Json::parse(base->canonicalJson())}}).value("ok",false),"initial generation");
 SetupSession session(store,call);
 check(!session.handle("status",{}).at("active").get<bool>()&&model.events.empty(),"view does not stop runtime");
 auto start=session.handle("start",{});const auto owner=start.at("session_id");
 check(model.events==std::vector<std::string>{"runtime_destroy","admin_load"},"destroy before admin load");
 check(rejected([&]{session.handle("start",{});}),"second writer rejected");
 check(!call({{"method","test"},{"text","включи свет на кухне"},{"execute",true},{"production",true}}).value("ok",false),"commands rejected during setup");
 model.raw=R"({"ops":[["location","Детская"],["power_device","Детская","Телевизор"],["power_device","Детская","Свет"]]})";
 auto message=Json{{"session_id",owner},{"version",0},{"request_id","first"},{"text","Создай детскую. Там добавь телевизор и свет только с включением и выключением."}};
 auto reply=session.handle("message",message);
 check(reply.at("configuration").at("devices").size()==2,"same draft contains devices");
 check(store.current()->contentHash()==base->contentHash(),"proposal never saves");
 for(const auto& d:reply.at("configuration").at("devices"))check(d.at("action_ids").size()==2&&d.at("state").size()==1,"power only capability exact");
 const int previousCalls=model.inferences;
 auto duplicate=session.handle("message",message);check(duplicate.at("version")==reply.at("version")&&model.inferences==previousCalls,"idempotent transport retry");
 model.raw=R"({"ops":[["rename_device","Детская","Свет","Основной свет"],["power_device","Детская","Ночник"]]})";
 reply=session.handle("message",{{"session_id",owner},{"version",reply.at("version")},{"request_id","second"},{"text","Свет назови основным. Ещё добавь ночник только с включением и выключением."}});
 check(reply.at("configuration").at("devices").size()==3,"followup extends draft");
 check(reply.at("configuration").at("devices")[1].at("name")=="Основной свет","rename follows previous draft");
 check(std::find(reply.at("configuration").at("devices")[1].at("spoken_aliases").begin(),reply.at("configuration").at("devices")[1].at("spoken_aliases").end(),"основной свет")!=reply.at("configuration").at("devices")[1].at("spoken_aliases").end(),"requested new name becomes a validated alias without overwriting existing language");
 model.raw=R"({"name":"Основной свет"})";
 check(!call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Основной свет назови настольным."},{"focus",Json::array()}}).value("ok",false),"unchanged rename cannot be presented as fulfilled");
 model.raw=R"({"ops":[["location","Детская"]]})";
 check(!call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Ещё добавь ночник."},{"focus",Json::array()}}).value("ok",false),"existing room cannot be recreated by a proposal");
 model.raw=R"({"ops":[["add_brightness","Детская","Основной свет"]]})";
 const auto added=call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Теперь добавь регулировку яркости основному свету"},{"focus",Json::array()}});
 check(added.value("ok",false),"explicit brightness extension accepted");
 if(added.value("ok",false))for(const auto& device:added.at("suggestion").at("configuration").at("devices"))if(device.at("name")=="Основной свет")
    check(device.at("action_ids").size()==3&&device.at("state").size()==2,"brightness addition preserves exact power actions");
 const auto forbidden=call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Основной свет только включать и выключать, без яркости"},{"focus",Json::array()}});
 check(!forbidden.value("ok",false),"model cannot add explicitly excluded brightness");
 check(store.current()->contentHash()==base->contentHash(),"capability preview never saves");
 const auto& proposedDevices=reply.at("configuration").at("devices");
 std::string nightId;for(const auto& d:proposedDevices)if(d.at("name")=="Ночник")nightId=d.at("id");
 const auto callsBeforeCancel=model.inferences;
 const auto undo=call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Ночник в детской пока не добавляй."},{"focus",Json::array({nightId})}});
 check(undo.value("ok",false)&&undo.at("suggestion").at("configuration").at("devices").size()==2,"uncommitted creation cancellation preserves other objects");
 check(model.inferences==callsBeforeCancel,"known uncommitted target cancellation needs no model guessing");
 const auto undoLast=call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Последнее устройство пока не добавляй."},{"focus",Json::array({proposedDevices[0].at("id"),nightId})}});
 check(undoLast.value("ok",false)&&undoLast.at("suggestion").at("configuration").at("devices").size()==2,"last-created exclusion follows application order not model guess");
 model.raw=R"({"ops":[["location","kabinet"]]})";
 const auto transliteration=call({{"method","dialogue"},{"configuration",reply.at("configuration")},{"text","Создай кабинет."},{"focus",Json::array()}});
 check(!transliteration.value("ok",false),"invented technical transliteration rejected");
 check(rejected([&]{session.handle("draft",{{"session_id",owner},{"version",0},{"configuration",reply.at("configuration")}});}),"stale manual version rejected");
 auto applied=session.handle("apply",{{"session_id",owner},{"version",reply.at("version")}});
 check(applied.at("status")=="applied"&&!session.active(),"apply and return runtime");
 check(store.current()->configuration().devices.size()==3,"saved graph");
 check(model.events==std::vector<std::string>{"runtime_destroy","admin_load","admin_destroy","runtime_load"},"single admin residency per dialogue");
 check(store.lastKnownGood().has_value(),"LKG after apply");
 check(applied.at("configuration").at("revision")==store.current()->configuration().revision,"Apply reply shows committed revision");
 {SetupSession completed(store,call);check(completed.handle("status",{}).at("status")=="applied","completed setup is not offered as pending after restart");
 check(rejected([&]{completed.handle("resume",{});}),"completed draft cannot be resumed as uncommitted");}

 const auto confirmed=store.current()->contentHash();
 auto next=session.handle("start",{});const auto nextOwner=next.at("session_id");
 check(rejected([&]{session.handle("heartbeat",{{"session_id",owner}});}),"previous owner token cannot mutate next session");
 check(rejected([&]{session.handle("cancel",{{"session_id",owner}});}),"previous owner cannot queue cancellation for a new session");
 session.tick();check(session.active()&&model.admin,"stale cancellation leaves current setup active");
 model.raw=R"({"name":"Экран"})";
 auto rename=session.handle("message",{{"session_id",nextOwner},{"version",0},{"request_id","rename"},{"text","Телевизор в детской назови экраном."}});
 check(rename.at("status")=="editing","rename locks full catalog target");
 auto question=session.handle("message",{{"session_id",nextOwner},{"version",rename.at("version")},{"request_id","pronoun"},{"text","Переименуй его."}});
 check(question.at("status")=="clarification_required","ambiguous pronoun asks clarification");
 model.raw=R"({"name":"Телевизор"})";
 auto clarified=session.handle("message",{{"session_id",nextOwner},{"version",question.at("version")},{"request_id","answer"},{"text","Экран в детской назови телевизором."}});
 check(clarified.at("status")=="editing","clarification resolves display name even without voice alias");
 model.raw="{broken";
 check(rejected([&]{session.handle("message",{{"session_id",nextOwner},{"version",clarified.at("version")},{"request_id","bad"},{"text","Добавь устройство"}});}),"malformed output rejected");
 check(store.current()->contentHash()==confirmed,"malformed proposal never saves");
 session.tick(std::chrono::steady_clock::now()+std::chrono::seconds(121));
 check(!session.active()&&!model.admin,"disconnect timeout restores runtime");
 check(store.current()->contentHash()==confirmed,"disconnect does not apply draft");
 SetupSession recovered(store,call);
 const auto recoveredView=recovered.handle("status",{});
 check(recoveredView.at("status")=="resume_required"&&!recoveredView.at("active").get<bool>(),"restart exposes draft without applying or loading model");
 const auto resumed=recovered.handle("resume",{});auto resumeOwner=resumed.at("session_id");
 check(resumeOwner!=nextOwner,"explicit resume rotates owner token");
 const auto eventsBefore=model.events.size();model.reject=true;
 check(rejected([&]{recovered.handle("cancel",{{"session_id",resumeOwner}});}),"restoration failure is explicit");
 check(recovered.handle("status",{}).at("status")=="restart_required","no false READY after restoration failure");
 const auto failedEvents=model.events.size();
 recovered.tick(std::chrono::steady_clock::now()+std::chrono::hours(2));
 check(model.events.size()==failedEvents&&failedEvents>eventsBefore,"failed restoration is not retried in a background loop");
 check(!call({{"method","test"},{"text","включи свет"}}).value("ok",false),"degraded mode still rejects commands");
 model.reject=false;recovered.handle("cancel",{{"session_id",resumeOwner}});
 check(!recovered.active(),"explicit restoration retry succeeds");
 auto pending=recovered.handle("start",{});auto pendingOwner=pending.at("session_id");
 model.raw=R"({"question":"Какое устройство?"})";
 model.block=true;model.release=false;model.entered=false;
 std::thread planning([&]{try{recovered.handle("message",{{"session_id",pendingOwner},{"version",0},{"request_id","blocked"},{"text","Переименуй устройство"}});}catch(...) {}});
 {std::unique_lock<std::mutex> lock(model.mutex);check(model.cv.wait_for(lock,std::chrono::seconds(3),[&]{return model.entered;}),"fake inference entered");}
 check(recovered.handle("status",{}).at("status")=="planning","read-only status remains available during inference");
 check(recovered.handle("cancel",{{"session_id",pendingOwner}}).at("status")=="cancel_pending","cancel during inference waits for accepted operation");
 {std::lock_guard<std::mutex> lock(model.mutex);model.release=true;model.cv.notify_all();}
 planning.join();model.block=false;recovered.tick();
 check(!recovered.active()&&!model.admin,"pending cancellation restores after synchronous worker finishes");
 check(store.current()->contentHash()==confirmed,"cancellation never publishes draft");
 struct stat info{};::stat((directory.path+"/setup-draft.json").c_str(),&info);
 check((info.st_mode&0777)==0600,"private persisted draft permissions");

 {
  admin_test::Directory isolated;firefly::config::ConfigStore rollbackStore(isolated.path);
  auto empty=rollbackStore.initialize(firefly::config::ConfigLoader::fromFile(std::string(argv[1])+"/config/empty-home.json"));
  SetupModel rollbackModel;RuntimeWorker rollbackWorker(rollbackModel);bool failNextApply=false;
  auto guarded=[&](const Json& request){
    if(failNextApply&&request.at("method")=="apply"){failNextApply=false;return Json{{"ok",false},{"errors",Json::array({"injected Apply failure after Save"})}};}
    return Json::parse(rollbackWorker.handle(request.dump()));
  };
  guarded({{"method","apply"},{"configuration",Json::parse(empty->canonicalJson())}});
  SetupSession rollbackSession(rollbackStore,guarded);auto state=rollbackSession.handle("start",{});const auto token=state.at("session_id");
  std::ifstream fixture(std::string(argv[1])+"/tests/language/home.json");const auto candidate=Json::parse(fixture);
  state=rollbackSession.handle("draft",{{"session_id",token},{"version",0},{"configuration",candidate}});
  check(rollbackStore.current()->configuration().devices.empty(),"manual draft shares session without active Save");
  failNextApply=true;
  check(rejected([&]{rollbackSession.handle("apply",{{"session_id",token},{"version",state.at("version")}});}),"post-Save Apply failure reported");
  check(rollbackSession.handle("status",{}).at("status")=="apply_failed_restored"&&!rollbackSession.active(),"post-Save failure restores assistant");
  check(rollbackStore.current()->configuration().devices.empty()&&rollbackStore.current()->configuration().revision>=3,"compensating CAS restores previous config without deleting store");
  check(rollbackStore.lastKnownGood().has_value(),"rollback records restored LKG");
  auto retry=rollbackSession.handle("start",{});const auto retryOwner=retry.at("session_id");
  rollbackSession.handle("cancel",{{"session_id",retryOwner}});
  const auto current=rollbackStore.current();rollbackStore.save(current,{current->configuration().revision,current->contentHash()});
  check(rejected([&]{rollbackSession.handle("resume",{});}),"stale base revision cannot resume old draft");
 }

 // Save succeeded, then Apply failed: compensate only our saved revision,
 // including deletions, never a later independent writer's revision.
 for(bool concurrentSave : {false,true}) {
  admin_test::Directory isolated;firefly::config::ConfigStore recoveryStore(isolated.path);
  const auto original=recoveryStore.initialize(admin_test::catalog(admin_test::bathroom(admin_test::base(argv[1]))));
  recoveryStore.recordLastKnownGood(original);
  SetupModel recoveryModel;RuntimeWorker recoveryWorker(recoveryModel);
  bool failApply=false;firefly::config::Catalog external;
  auto guarded=[&](const Json& request) {
   if(failApply && request.at("method")=="apply") {
    failApply=false;
    if(concurrentSave) {
     const auto saved=recoveryStore.current();auto edit=Json::parse(saved->canonicalJson());
     edit["system"]["name"]="Concurrent confirmed edit";
     external=recoveryStore.save(admin_test::catalog(edit),{saved->configuration().revision,saved->contentHash()});
    }
    return Json{{"ok",false},{"errors",Json::array({"injected Apply failure"})}};
   }
   return Json::parse(recoveryWorker.handle(request.dump()));
  };
  guarded({{"method","apply"},{"configuration",Json::parse(original->canonicalJson())}});
  SetupSession recovery(recoveryStore,guarded);auto state=recovery.handle("start",{});const auto token=state.at("session_id");
  auto candidate=Json::parse(original->canonicalJson());
  for(const auto* category:{"rooms","devices"}) {
   const std::string removed=std::string(category)=="rooms"?"room_bathroom":"device_bathroom_light";
   auto& entries=candidate[category];entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const Json& item){return item.at("id")==removed;}),entries.end());
  }
  state=recovery.handle("draft",{{"session_id",token},{"version",0},{"configuration",candidate}});
  failApply=true;
  check(rejected([&]{recovery.handle("apply",{{"session_id",token},{"version",state.at("version")}});}),"failed Apply is reported after deletion Save");
  if(concurrentSave) {
   check(recoveryStore.current()->contentHash()==external->contentHash(),"rollback CAS never overwrites concurrent confirmed Save");
   check(recoveryStore.lastKnownGood().value()->contentHash()==original->contentHash(),"CAS conflict does not record false LKG");
   check(recovery.handle("status",{}).at("status")=="restart_required"&&recoveryWorker.setupBlocked(),"concurrent rollback conflict stays explicitly unavailable");
  } else {
   const auto restored=recoveryStore.current();
   check(restored->deviceIndex().count("device_bathroom_light")&&restored->roomIndex().count("room_bathroom"),"compensation restores deleted room and device");
   check(restored->configuration().retiredIds.empty(),"only failed transaction retirements are undone");
   check(recoveryStore.lastKnownGood().value()->contentHash()==restored->contentHash(),"restored configuration recorded as LKG");
   const auto live=Json::parse(recoveryWorker.observe());
   check(live.at("active").at("configuration")==Json::parse(restored->canonicalJson()),"runtime and restored store are the same generation");
   check(recovery.handle("status",{}).at("status")=="apply_failed_restored"&&!recoveryWorker.setupBlocked(),"deletion rollback restores assistant without restart");
  }
 }

 // Abrupt process exit after the first sentence has been persisted. No
 // exception handler runs, so this exercises the actual recovery boundary.
 {
  admin_test::Directory isolated;firefly::config::ConfigStore crashStore(isolated.path);
  const auto original=crashStore.initialize(firefly::config::ConfigLoader::fromFile(std::string(argv[1])+"/config/empty-home.json"));
  const std::string text="Создай кабинет. Там добавь свет только включать и выключать.";
  auto proposal=[](const Json& request,int step) {
   const auto wire=step==1?R"({"ops":[["location","Кабинет"]]})":R"({"ops":[["power_device","Кабинет","Свет"]]})";
   return Json{{"ok",true},{"suggestion",Json::parse(decodeCopilotProposal(expandDialogueOperations(wire).dump(),admin_test::catalog(request.at("configuration"))))}};
  };
  const auto child=::fork();if(child<0)throw std::runtime_error("fork failed");
  if(child==0) {
   int steps=0;
   auto crashCall=[&](const Json& request)->Json {
    if(request.at("method")=="dialogue") {if(++steps==2)::_exit(73);return proposal(request,steps);}
    if(request.at("method")=="language_profile")return {{"ok",false},{"errors",Json::array()}};
    return {{"ok",true}};
   };
   try {
    SetupSession interrupted(crashStore,crashCall);const auto state=interrupted.handle("start",{});
    interrupted.handle("message",{{"session_id",state.at("session_id")},{"version",0},{"request_id","interrupted"},{"text",text}});
   } catch(...) {::_exit(74);}
   ::_exit(75);
  }
  int status=0;::waitpid(child,&status,0);
  check(WIFEXITED(status)&&WEXITSTATUS(status)==73,"crash occurs during second dialogue step");
  std::ifstream persisted(isolated.path+"/setup-draft.json");const auto partial=Json::parse(persisted);
  check(partial.at("configuration").at("rooms").size()==1 && partial.at("message_incomplete")==true,"partial first step persisted with incomplete marker");
  int retrySteps=0,applies=0;
  auto call=[&](const Json& request)->Json {
   if(request.at("method")=="dialogue")return proposal(request,++retrySteps);
   if(request.at("method")=="language_profile")return {{"ok",false},{"errors",Json::array()}};
   if(request.at("method")=="apply")++applies;
   return {{"ok",true}};
  };
  {
   SetupSession resumed(crashStore,call);auto state=resumed.handle("resume",{});const auto token=state.at("session_id");
   check(state.at("status")=="planning_failed"&&state.at("configuration").at("rooms").empty(),"resume restores pre-message checkpoint and exposes incompleteness");
   check(rejected([&]{resumed.handle("apply",{{"session_id",token},{"version",state.at("version")}});}),"resumed partial message cannot Apply");
   check(rejected([&]{resumed.handle("draft",{{"session_id",token},{"version",state.at("version")},{"configuration",partial.at("configuration")}});}),"manual draft cannot erase incompleteness");
   check(rejected([&]{resumed.handle("message",{{"session_id",token},{"version",state.at("version")},{"request_id","unrelated"},{"text","Добавь ночник"}});}),"unrelated message cannot silently complete unfinished request");
   resumed.handle("cancel",{{"session_id",token}});
  }
  SetupSession resumedAgain(crashStore,call);auto state=resumedAgain.handle("resume",{});const auto token=state.at("session_id");
  check(state.at("message_incomplete")==true&&applies==0&&crashStore.current()->contentHash()==original->contentHash(),"cancel/restart/resume retains incompleteness without automatic Apply");
  state=resumedAgain.handle("message",{{"session_id",token},{"version",state.at("version")},{"request_id","explicit-retry"},{"text",text}});
  check(state.at("status")=="editing"&&state.at("message_incomplete")==false&&retrySteps==2,"explicit complete retry resolves both original steps");
  check(state.at("configuration").at("rooms").size()==1&&state.at("configuration").at("devices").size()==1,"retry does not duplicate partially persisted creations");
  resumedAgain.handle("apply",{{"session_id",token},{"version",state.at("version")}});
  check(applies==1&&crashStore.current()->configuration().devices.size()==1,"only completed request can be explicitly applied");
 }

 }catch(const std::exception&e){++failures;std::cerr<<"EXCEPTION "<<e.what()<<'\n';}
 std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
