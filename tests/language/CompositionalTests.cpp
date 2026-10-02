#include "../admin/Support.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/voice/VoiceProcessor.h"
#include <iostream>
using namespace firefly;
using admin_test::Json;
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
struct Model:admin_test::Model {
    llm::CommandProtocol commandProtocol()const override{return llm::CommandProtocol::StableCompact;}
};
struct Transcript:voice::SpeechRecognizer {
    std::string text;
    voice::RecognitionResult recognize(const std::vector<float>&)override{return {true,text,"",0};}
};
config::Catalog home(const std::string& root,const std::string& roomName) {
    const auto empty=config::ConfigLoader::fromFile(root+"/config/empty-home.json");
    const Json proposal={{"status","ok"},{"summary","Создать комнату и два устройства только с питанием"},
        {"operations",Json::array({{{"op","create_location"},{"name",roomName},{"type","room"},{"description",roomName}},
          {{"op","create_device"},{"location",roomName},{"name","Свет"},{"template","power_only"},{"description","Свет"}},
          {{"op","create_device"},{"location",roomName},{"name","Телевизор"},{"template","power_only"},{"description","Телевизор"}}})}};
    const auto decoded=Json::parse(admin::decodeCopilotProposal(proposal.dump(),empty));
    check(decoded.contains("configuration"),"Admin proposal rejected");
    return admin_test::catalog(decoded.at("configuration"));
}
int main(int argc,char**argv){try{
    if(argc!=2)return 2;const std::string root=argv[1];
    for(const auto& forms:std::vector<std::vector<std::string>>{{"Детская комната","детской","детском","детским"},
        {"Читальная комната","читальной","читальном","читальным"},{"Бирюзовая комната","бирюзовой","бирюзовом","бирюзовым"},{"Синяя комната","синей","синем","синим"}}) {
        const auto catalog=home(root,forms[0]);Model model;model.raw="[]";
        admin::RuntimeGenerations runtime(model);runtime.apply(catalog);
        std::string light,tv,on,off;
        for(const auto& d:catalog->configuration().devices){if(d.name=="Свет")light=d.id;else tv=d.id;}
        const auto& room=catalog->configuration().rooms[0];
        const std::vector<std::string> corpus={"Включи свет в "+forms[1],"Включи свет в "+forms[1]+" комнате",
            "В "+forms[1]+" включи свет","Свет в "+forms[1]+" включи","Пожалуйста включи свет в "+forms[1],
            "Включи свет на "+forms[2],"Выключи свет в "+forms[1],"Включи телевизор в "+forms[1],"Выключи телевизор в "+forms[1]};
        for(const auto& text:corpus) {
            const auto count=model.inferences;const auto r=runtime.process(text).command;
            check(r.languageRoute=="INDEX_EXACT"&&std::holds_alternative<actions::GenericAction>(r.action),text+": "+r.indexMatch.reason);
            check(!r.safetyRejected&&model.inferences==count,text+": Safety/model count");
            const auto& action=std::get<actions::GenericAction>(r.action);
            check(action.targetId()==(text.find("телевизор")==std::string::npos?light:tv),"wrong device");
            const auto& definition=catalog->configuration().actions.at(catalog->actionIndex().at(action.actionId()));
            check(definition.parameters.empty()&&definition.assignments.size()==1,"unrequested capability");
            check(definition.assignments[0].value==actions::Scalar(text.find("Выключи")==std::string::npos),"wrong power value");
            check(r.grounding.locations==std::vector<std::string>{room.id},"location was ignored");
        }
        for(const auto& text:std::vector<std::string>{"Не включи свет в "+forms[1],"Включи свет в "+forms[1]+" на 40 процентов",
            "Включи свет в "+forms[1]+" и телевизор","Включи свет в "+forms[1]+" или телевизор",
            "Включи свет на "+forms[2]+" столе","Включи свет в "+forms[1]+" через час",
            "Если темно включи свет в "+forms[1],"Выключи свет в "+forms[1]+" завтра"}) {
            const auto count=model.inferences;const auto r=runtime.process(text).command;
            check(std::holds_alternative<actions::Unsupported>(r.action),"unsafe accepted: "+text);
            check(model.inferences==count,"unsafe grammar was handed to model: "+text);
        }
        check(runtime.current()->languageIndex->match("Вклюци свет в "+forms[1]).kind==language::MatchKind::Miss,"fuzzy action");
        check(runtime.current()->domain.resolve("Включи свет "+forms[3]).locations.empty(),"bare derived adjective was invented as a location reference");
        const auto original=runtime.current();
        auto changed=Json::parse(catalog->canonicalJson());
        changed["rooms"][0]["speech"]={{"mentions",{"дальняя комната"}},{"locations",{"в дальней комнате"}}};
        changed["rooms"][0]["metadata"]["aliases"]=Json::array();changed["rooms"][0]["name"]="Дальняя комната";
        runtime.apply(admin_test::catalog(changed));
        check(runtime.current()->domain.resolve(corpus[0]).locations.empty(),"old grammar survived rename");
        check(original->languageIndex->match(corpus[0]).kind==language::MatchKind::ExactUnique,"old generation mutated");
        check(runtime.current()->languageIndex->match("включи свет в дальней комнате").kind==language::MatchKind::ExactUnique,"new grammar missing");
        changed["devices"]=Json::array();changed["rooms"]=Json::array();runtime.apply(admin_test::catalog(changed));
        check(runtime.current()->languageIndex->match(corpus[0]).kind==language::MatchKind::Miss,"deleted entities still match");

        // Actual post-STT voice entry, not a separate hand-built CommandEngine.
        admin::RuntimeWorker worker(model);
        check(Json::parse(worker.handle(Json{{"method","apply"},{"configuration",Json::parse(catalog->canonicalJson())}}.dump())).value("ok",false),"voice generation apply");
        Transcript transcript;transcript.text=corpus[0];voice::Utterance utterance{{0,16000},std::vector<float>(16000),"",false};
        const auto count=model.inferences;const auto voice=worker.processVoice(transcript,utterance);
        check(std::holds_alternative<actions::GenericAction>(voice.action)&&model.inferences==count,"voice fast path used model");
        const auto status=Json::parse(worker.observe());const auto command=status.at("last_voice").at("command");
        check(command.at("language_route")=="INDEX_EXACT"&&command.at("safety")=="PASS","voice route/safety");
        check(command.at("execution").at("error")=="binding_missing","no hardware or mock execution without binding");
        check(command.at("language_index").at("hash")==status.at("active").at("hash"),"voice mixed generation");
    }
    const auto catalog=home(root,"Детская комната");auto two=Json::parse(catalog->canonicalJson());
    auto room=two["rooms"][0];room["id"]="second_location";room["name"]="Голубая комната";
    room["speech"]={{"mentions",{"голубая комната"}},{"locations",{"в голубой комнате"}}};room["metadata"]["aliases"]=Json::array();two["rooms"].push_back(room);
    const auto devices=two["devices"];for(auto device:devices){device["id"]="second_"+device["id"].get<std::string>();device["room_id"]=room["id"];two["devices"].push_back(device);}
    Model model;model.raw="[]";admin::RuntimeGenerations runtime(model);runtime.apply(admin_test::catalog(two));
    auto count=model.inferences;
    check(runtime.process("Включи свет").command.indexStatus=="INDEX_AMBIGUOUS"&&model.inferences==count,"shared name guessed");
    check(runtime.process("Включи свет в детской или голубой комнате").command.indexStatus=="INDEX_AMBIGUOUS"&&model.inferences==count,"alternative room guessed");
    for(const auto& text:{"Включи свет в детской","Включи свет в голубой комнате"})
        check(std::holds_alternative<actions::GenericAction>(runtime.process(text).command.action)&&model.inferences==count,"shared name not disambiguated");
    // Two configured adjective names may generate the same grammatical form.
    two["rooms"][1]["speech"]={{"mentions",{"детский"}},{"locations",{"на детском"}}};
    two["rooms"][1]["metadata"]["aliases"]={"детской"};runtime.apply(admin_test::catalog(two));
    check(runtime.process("Включи свет в детской").command.indexStatus=="INDEX_AMBIGUOUS"&&model.inferences==count,"derived/exact collision selected a first room");
    check(runtime.process("Включи свет на детском").command.indexStatus=="INDEX_AMBIGUOUS"&&model.inferences==count,"explicit preposition hid conflicting grammatical location");
    check(std::any_of(runtime.current()->languageIndex->conflicts().begin(),runtime.current()->languageIndex->conflicts().end(),[](const auto& c){return c.code=="location_grammar_ambiguity";}),"derived conflict not previewed");
    // Suggestions retain reviewed seeds; capabilities/bindings cannot enter codec.
    const auto loc=catalog->configuration().rooms[0].id;
    const auto p=Json::parse(admin::decodeLanguageProfile(R"({"aliases":["детская"],"mentions":["детской"],"locations":["в детской"]})",catalog,"location",loc));
    check(std::find(p["mentions"].begin(),p["mentions"].end(),"детская комната")!=p["mentions"].end(),"AI erased canonical seed");
    std::cout<<"PASS compositional Admin atoms, 4 arbitrary rooms, voice path, safety and generation regressions\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
