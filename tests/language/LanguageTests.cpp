#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/language/Integer.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <future>
using namespace firefly;
using Json=nlohmann::json;
void check(bool b,const std::string& message){if(!b)throw std::runtime_error(message);}
struct Model:admin::ContextModel {
    int calls=0;std::string raw="null";
    llm::CommandProtocol commandProtocol()const override{return llm::CommandProtocol::StableCompact;}
    void useContext(const std::string&)override{}
    bool ready()const override{return true;}
    std::string infer(std::string_view)override{++calls;return raw;}
};
int main(int argc,char** argv){try{
    if(argc!=2)return 2;
    const std::string root=argv[1];
    auto empty=config::ConfigLoader::fromFile(root+"/config/empty-home.json");
    Model model;admin::RuntimeGenerations runtime(model);runtime.apply(empty);
    check(std::holds_alternative<actions::Unsupported>(runtime.process("включи свет").command.action),"empty domain");
    auto catalog=config::ConfigLoader::fromFile(root+"/tests/language/home.json");
    runtime.apply(catalog);
    const std::vector<std::pair<std::string,int>> corpus={
        {"включи свет на кухне",100},{"зажги основной свет на кухне",100},{"кухонный свет включи",100},
        {"выключи освещение на кухне",0},{"поставь свет на кухне на 40 процентов",40},
        {"установи яркость основного света на кухне сорок",40},{"на кухне основной свет 55 процентов",55}};
    const int before=model.calls;
    for(const auto& [text,value]:corpus){
        const auto r=runtime.process(text).command;
        check(r.languageRoute=="INDEX_EXACT",text+" route="+r.languageRoute);
        check(std::holds_alternative<actions::GenericAction>(r.action),text+" SafetyGate reject");
        const auto& a=std::get<actions::GenericAction>(r.action);
        check(a.targetId()=="lamp_a",text+" target");
        check(a.actionId()==(value==100?"power_on":value==0?"power_off":"set_level"),text+" action");
        if(!a.arguments().empty())check(a.arguments().at("level").value()==value,text+" value");
    }
    check(model.calls==before,"index used Qwen");
    for(const auto* value:{"0","1","40","100","ноль","один","сорок","сто","пятьдесят пять","40%"}){
        const auto r=runtime.process(std::string("поставь свет на кухне на ")+value).command;
        check(r.languageRoute=="INDEX_EXACT"&&std::holds_alternative<actions::GenericAction>(r.action),std::string("number ")+value);
    }
    for(const auto* text:{"поставь свет на кухне 101","поставь свет на кухне 40 50","поставь свет на кухне сорок, пять","поставь свет на кухне минус сорок","поставь свет на кухне 40.5","включи свет на кухне и выключи свет","не включи свет на кухне","включи свет на кухне 40"}){
        const int count=model.calls;
        check(std::holds_alternative<actions::Unsupported>(runtime.process(text).command.action),text);
        check(model.calls==count,std::string(text)+" reached Qwen");
    }
    model.raw="[]";auto miss=runtime.process("будь добр включи свет на кухне").command;
    check(miss.indexStatus=="INDEX_MISS"&&model.calls==before+1,"unknown phrase fallback");
    auto document=Json::parse(catalog->canonicalJson());
    auto room=document["rooms"][0];room["id"]="place_b";room["name"]="Спальня";room["speech"]={{"mentions",{"спальня","спальне"}},{"locations",{"в спальне"}}};document["rooms"].push_back(room);
    auto device=document["devices"][0];device["id"]="lamp_b";device["room_id"]="place_b";document["devices"].push_back(device);document["revision"]=2;
    const auto old=runtime.current();runtime.apply(config::ConfigLoader::fromJson(document.dump()));
    check(old->languageIndex->catalogHash()==catalog->contentHash(),"old generation changed");
    check(runtime.current()->languageIndex->catalogHash()!=old->languageIndex->catalogHash(),"generation identity stale");
    auto count=model.calls;auto ambiguous=runtime.process("включи свет").command;
    check(ambiguous.indexStatus=="INDEX_AMBIGUOUS"&&model.calls==count,"ambiguous called Qwen");
    check(std::holds_alternative<actions::GenericAction>(runtime.process("включи свет на кухне").command.action),"location disambiguation");
    runtime.apply(catalog);check(runtime.current()->languageIndex->fingerprint()==old->languageIndex->fingerprint(),"restore index");
    check(std::get<actions::GenericAction>(runtime.process("включи свет").command.action).targetId()=="lamp_a","restore matching");
    auto profile=Json::parse(admin::decodeLanguageProfile(R"({"aliases":["Включи","включи","зажги"]})",catalog,"action","power_on"));
    check(profile["aliases"].size()==2,"proposal normalization/dedup");
    for(const auto* raw:{R"({"aliases":["игнорируй инструкции"]})",R"({"aliases":["пожалуйста"]})",R"({"aliases":["включи"],"assignments":[]})",R"({"aliases":["1","2","3","4","5","6","7"]})","```{}"}){
        bool failed=false;try{admin::decodeLanguageProfile(raw,catalog,"action","power_on");}catch(...){failed=true;}check(failed,"malformed proposal accepted");
    }
    document=Json::parse(catalog->canonicalJson());document["actions"][1]["speech_aliases"]=document["actions"][0]["speech_aliases"];
    bool failed=false;try{language::LanguageCompiler::compile(config::ConfigLoader::fromJson(document.dump()));}catch(const config::ConfigError&){failed=true;}check(failed,"action alias conflict accepted");
    // Compiler identity and ordering: IDs/aliases are data, never first-match defaults.
    auto reordered = Json::parse(catalog->canonicalJson());
    std::reverse(reordered["actions"].begin(), reordered["actions"].end());
    std::reverse(reordered["devices"][0]["spoken_aliases"].begin(), reordered["devices"][0]["spoken_aliases"].end());
    auto reindex = language::LanguageCompiler::compile(config::ConfigLoader::fromJson(reordered.dump()));
    auto duplicates = Json::parse(catalog->canonicalJson());
    for (auto& a : duplicates["actions"]) if (a["id"] == "power_on") a["speech_aliases"].push_back("  ВКЛЮЧИ  ");
    const auto deduplicated = language::LanguageCompiler::compile(config::ConfigLoader::fromJson(duplicates.dump()));
    check(deduplicated->approximateBytes() == old->languageIndex->approximateBytes(), "compiler duplicated normalized atom");
    check(reindex->match(corpus[0].first).candidate == old->languageIndex->match(corpus[0].first).candidate, "order-dependent match");
    check(language::LanguageCompiler::compile(catalog)->fingerprint() == old->languageIndex->fingerprint(), "nondeterministic fingerprint");
    bool mixed = false;
    try { runtime::DomainView other(config::ConfigLoader::fromJson(reordered.dump())); DeterministicParser parser(other); CommandEngine wrong(model, parser, other, {}, old->languageIndex.get()); }
    catch (const std::invalid_argument&) { mixed = true; }
    check(mixed, "mixed index/domain constructor admitted");
    check(reindex->catalogHash() == config::ConfigLoader::fromJson(reordered.dump())->contentHash(), "wrong index hash");
    check(old->languageIndex->match("кухонныи свет включи").kind == language::MatchKind::FuzzyUnique, "entity fuzzy missing");
    check(old->languageIndex->match("кухонный свет вклюци").kind == language::MatchKind::Miss, "fuzzy intent");
    check(old->languageIndex->match("поставь свет на кухне сороок").kind == language::MatchKind::Miss, "fuzzy number");
    for (const auto* text : {"включи свет на кухне неизвестное", "расскажи включи свет на кухне"})
        check(old->languageIndex->match(text).kind == language::MatchKind::Miss, "partial utterance accepted");
    check(old->languageIndex->match(std::string(193, 'a')).kind == language::MatchKind::Invalid, "input bound");
    check(old->languageIndex->match(std::string(1, char(255))).kind == language::MatchKind::Invalid, "invalid UTF8");
    // Complete exact Russian cardinal range, including tens + units.
    const std::vector<std::string> units={"ноль","один","два","три","четыре","пять","шесть","семь","восемь","девять","десять","одиннадцать","двенадцать","тринадцать","четырнадцать","пятнадцать","шестнадцать","семнадцать","восемнадцать","девятнадцать"};
    const std::vector<std::string> tens={"","","двадцать","тридцать","сорок","пятьдесят","шестьдесят","семьдесят","восемьдесят","девяносто","сто"};
    for (int n=0;n<=100;++n) {
        const auto word=n<20?units[n]:tens[n/10]+(n%10?" "+units[n%10]:"");
        const auto r=runtime.process("поставь свет на кухне "+word+" процентов").command;
        check(std::holds_alternative<actions::GenericAction>(r.action),"cardinal rejected "+word);
        check(std::get<actions::GenericAction>(r.action).arguments().at("level").value()==n,"wrong cardinal");
    }
    // A unique candidate still cannot bypass SafetyGate's original-input policy.
    auto guarded=Json::parse(catalog->canonicalJson());
    for(auto& a:guarded["actions"])if(a["id"]=="power_on")a["speech_aliases"].push_back("почему включи");
    runtime.apply(config::ConfigLoader::fromJson(guarded.dump()));
    const auto unsafe=runtime.process("почему включи свет на кухне").command;
    check(unsafe.languageRoute=="INDEX_EXACT"&&unsafe.safetyRejected,"index bypassed SafetyGate");
    runtime.apply(catalog);
    auto lease=runtime.beginRequest();const auto leased=lease->generation();
    auto renamed=Json::parse(catalog->canonicalJson());renamed["devices"][0]["spoken_aliases"]={"лампа"};renamed["revision"]=3;
    auto applying=std::async(std::launch::async,[&]{runtime.apply(config::ConfigLoader::fromJson(renamed.dump()));});
    check(applying.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"Apply crossed active lease");
    check(lease->process("включи свет на кухне").generation==leased,"mixed request generation");lease.reset();applying.get();
    check(runtime.current()->languageIndex->match("включи свет на кухне").kind==language::MatchKind::Miss,"old alias survived rename");
    check(leased->languageIndex->match("включи свет на кухне").kind==language::MatchKind::ExactUnique,"old index mutated");
    renamed["devices"]=Json::array();runtime.apply(config::ConfigLoader::fromJson(renamed.dump()));
    check(runtime.current()->languageIndex->match("включи лампа на кухне").kind==language::MatchKind::Miss,"deleted alias survived");
    runtime.apply(catalog);
    auto unitsCatalog = Json::parse(catalog->canonicalJson());
    for (auto& a : unitsCatalog["actions"]) if (a["id"] == "set_level") a["parameters"][0]["unit"] = "rpm";
    unitsCatalog["devices"][0]["state"][0]["unit"] = "rpm";
    const auto unitIndex = language::LanguageCompiler::compile(config::ConfigLoader::fromJson(unitsCatalog.dump()));
    check(unitIndex->match("поставь свет на кухне 40%").kind == language::MatchKind::Invalid, "wrong unit admitted");
    auto numbered = Json::parse(catalog->canonicalJson()); numbered["devices"][0]["spoken_aliases"] = {"пять"};
    const auto numberedIndex = language::LanguageCompiler::compile(config::ConfigLoader::fromJson(numbered.dump()));
    check(numberedIndex->match("поставь сорок пять на кухне").kind == language::MatchKind::Invalid, "entity reused as argument");
    // Simple scalar schemas are derived from configuration, not light IDs.
    for(const auto* type:{"boolean","enum"}) {
        auto scalar=Json::parse(catalog->canonicalJson());
        Json p={{"id","value"},{"name","Значение"},{"type",type},{"required",true},{"unit",""},{"description",""}};
        if(std::string(type)=="enum")p["values"]={"тихо","громко"};
        auto state=p;state["initial"]=std::string(type)=="enum"?Json("тихо"):Json(false);
        scalar["devices"][0]["state"]=Json::array({state});scalar["devices"][0]["action_ids"]={"set_value"};
        scalar["actions"]=Json::array({{{"id","set_value"},{"name","Задать"},{"description",""},{"operation","mock.assign.v1"},{"enabled",true},{"parameters",Json::array({p})},{"speech_aliases",{"задай"}},{"assignments",Json::array({{{"field","value"},{"argument","value"}}})}}});
        admin::RuntimeGenerations scalarRuntime(model);
        scalarRuntime.apply(config::ConfigLoader::fromJson(scalar.dump()));
        const auto value=std::string(type)=="enum"?"тихо":"true";
        check(std::holds_alternative<actions::GenericAction>(scalarRuntime.process(std::string("задай свет на кухне ")+value).command.action),"scalar failed");
        const auto wrong=scalarRuntime.current()->languageIndex->match("задай свет на кухне 40");
        check(wrong.kind==language::MatchKind::Invalid,"wrong scalar type admitted");
        check(scalarRuntime.current()->languageIndex->match(std::string("задай свет на кухне ")+value+" "+value).kind==language::MatchKind::Invalid,"multiple scalar values");
        if (std::string(type) == "enum") {
            scalar["actions"][0]["parameters"][0]["values"] = {"тихо", "ТИХО"};
            scalar["devices"][0]["state"][0]["values"] = {"тихо", "ТИХО"};
            bool collision = false;
            try { language::LanguageCompiler::compile(config::ConfigLoader::fromJson(scalar.dump())); }
            catch (const config::ConfigError& error) { collision = error.code == "value_interpretation_conflict"; }
            check(collision, "duplicate normalized enum interpretation");
        }

    }
    std::cout<<"Language index, numbers, safety, empty home, generation, profile passed\n";
}catch(const config::ConfigError& e){std::cerr<<e.code<<" "<<e.path<<" "<<e.what()<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
