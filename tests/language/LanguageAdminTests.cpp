#include "../admin/Support.h"
#include "firefly/admin/AdminDraft.h"
#include <iostream>
using namespace firefly;
using namespace admin_test;
void check(bool x,const char* s){if(!x)throw std::runtime_error(s);}
int main(int argc,char** argv){try{
    if(argc!=2)return 2;
    const std::string root=argv[1];Directory dir;config::ConfigStore store(dir.path);
    store.initialize(config::ConfigLoader::fromFile(root+"/config/empty-home.json"));
    Model model;Client client(model);admin::Api api(store,client);api.initializeRuntime();
    auto call=[&](const std::string& method,const std::string& path,const Json& body=Json()){
        auto reply=api.handle(method,path,body.is_null()?"":body.dump());return std::pair<int,Json>{reply.status,Json::parse(reply.body)};
    };
    auto identity=[&]{auto c=store.current();return Json{{"revision",c->configuration().revision},{"hash",c->contentHash()}};};
    const auto initial=identity();
    check(call("GET","/api/status").second["runtime_ready"],"empty runtime not ready");
    const auto home=config::ConfigLoader::fromFile(root+"/tests/language/home.json");
    auto document=Json::parse(home->canonicalJson());
    model.raw=R"({"aliases":["Включи","зажги","включи"]})";
    auto proposal=call("POST","/api/admin/language-profile",{{"configuration",document},{"entity","action"},{"id","power_on"}});
    check(proposal.first==200&&proposal.second["suggestion"]["aliases"].size()==3,"action proposal API preserves canonical atoms");
    check(proposal.second["saved"]==false&&proposal.second["applied"]==false&&identity()==initial,"proposal autosaved");
    model.raw=R"({"aliases":["выключи"]})";
    proposal=call("POST","/api/admin/language-profile",{{"configuration",document},{"entity","action"},{"id","power_on"}});
    check(proposal.second["suggestion"]["conflicts"].size()>0,"proposal conflicts missing");
    auto bad=document;for(auto& a:bad["actions"])if(a["id"]=="power_on")a["speech_aliases"]={"выключи"};
    check(call("POST","/api/config/validate",{{"configuration",bad}}).first!=200,"Validate admits conflict");
    check(call("PUT","/api/config",{{"configuration",bad},{"expected",identity()}}).first!=200,"Save admits conflict");
    check(identity()==initial,"failed validation saved");
    const auto locationId=document.at("rooms")[0].at("id");
    model.raw=R"({"aliases":["кухня"],"mentions":["кухня"],"locations":["кухне"]})";
    check(call("POST","/api/admin/language-profile",{{"configuration",document},{"entity","location"},{"id",locationId}}).first==422,"Bare noun accepted as a complete location phrase");
    model.raw=R"({"aliases":["кухня"],"mentions":["кухня"],"locations":["на кухне"]})";
    check(call("POST","/api/admin/language-profile",{{"configuration",document},{"entity","location"},{"id",locationId},{"initial_location",true}}).first!=200,"External caller could replace reviewed seed forms");
    check(identity()==initial,"Language validation changed the store");
    model.raw="null";
    check(call("POST","/api/config/validate",{{"configuration",document}}).first==200,"home validate");
    check(call("PUT","/api/config",{{"configuration",document},{"expected",identity()}}).first==200,"home save");
    check(call("POST","/api/config/apply",{{"expected",identity()}}).first==200,"home apply");
    const int count=model.inferences;
    auto command=call("POST","/api/test-command",{{"text","установи яркость основного света на кухне сорок"},{"execute",true}});
    check(command.first==200&&command.second["language_route"]=="INDEX_EXACT"&&command.second["safety"]=="PASS","API index route/safety");
    check(command.second["execution"]["success"]==true&&model.inferences==count,"mock execution used Qwen");
    const auto homeRevision = store.current()->configuration().revision;
    auto renamed = document; renamed["devices"][0]["spoken_aliases"] = {"лампа"};
    check(call("PUT","/api/config",{{"configuration",renamed},{"expected",identity()}}).first==200,"alias save");
    check(call("POST","/api/config/apply",{{"expected",identity()}}).first==200,"alias apply");
    check(call("POST","/api/test-command",{{"text","включи свет на кухне"}}).second["result"]["unsupported"]==true,"removed alias remains active");
    check(call("POST","/api/config/restore",{{"revision",homeRevision},{"expected",identity()}}).first==200,"revision restore");
    check(call("POST","/api/config/apply",{{"expected",identity()}}).first==200,"restored apply");
    command=call("POST","/api/test-command",{{"text","включи свет на кухне"}});
    check(command.second["language_route"]=="INDEX_EXACT"&&command.second["language_index"]["hash"]==identity()["hash"],"restored index identity");
    // Existing plan/Copilot templates must work without legacy compatibility IDs.
    const auto empty=config::ConfigLoader::fromFile(root+"/config/empty-home.json");
    auto plan=Json::parse(admin::decodePlan(R"({"location":{"name":"Кухня","type":"room","description":"Кухня","mentions":["кухне"],"locations":["на кухне"]},"devices":[{"name":"Свет","template":"light","aliases":["свет"],"description":"Свет"}]})",empty));
    check(plan["configuration"]["devices"][0]["kind"]=="generic","empty plan light template");
    admin::RuntimeGeneration admitted(config::ConfigLoader::fromJson(plan["configuration"].dump()));
    std::cout<<"Language Admin empty/create/proposal/conflict/validate/save/apply/execution passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
