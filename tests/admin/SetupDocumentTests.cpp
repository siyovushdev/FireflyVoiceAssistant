#include "Support.h"
#include "AdminSetupDocument.h"
#include "firefly/config/ConfigStore.h"
#include <iostream>
using namespace firefly::admin;
using admin_test::Json;
int main(int argc,char** argv) {
    int checks=0,failures=0;
    const auto check=[&](bool value,const char* name){++checks;if(!value){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    const auto rejects=[](const auto& operation){try{operation();return false;}catch(const std::exception&){return true;}};
    try {
        if(argc!=2)throw std::runtime_error("source root required");
        auto empty=firefly::config::ConfigLoader::fromFile(std::string(argv[1])+"/config/empty-home.json");
        admin_test::Directory directory;firefly::config::ConfigStore store(directory.path);store.initialize(empty);
        const auto baseHash=store.current()->contentHash();
        Json wire={{"ops",Json::array({Json::array({"location","Кухня"}),Json::array({"power_device","Кухня","Свет"}),Json::array({"location","Кабинет"}),Json::array({"power_device","Кабинет","Лампа"})})},{"complete",true}};
        std::string input="У меня кухня и кабинет. Добавь Кухня, Свет, Кабинет, Лампа; оба устройства только включать и выключать. ";
        for(int i=0;i<20;++i)input+="Я описываю существующие помещения и хочу сначала увидеть весь план, без отправки команд и без автоматического применения.\n";
        auto result=Json::parse(decodeSetupDocument(wire.dump(),empty,input));
        check(result.at("configuration").at("rooms").size()==2&&result.at("configuration").at("devices").size()==2,"long Russian whole document creates complete multi-zone plan");
        check(result.at("proposal_scope")=="configuration"&&!result.at("saved")&&!result.at("applied"),"private configuration proposal markers");
        auto missing=wire;missing.erase("complete");check(rejects([&]{decodeSetupDocument(missing.dump(),empty,input);}),"long document missing completeness acknowledgement rejected");
        missing=wire;missing["complete"]=false;check(rejects([&]{decodeSetupDocument(missing.dump(),empty,input);}),"explicit incomplete result rejected");
        missing=wire;missing["complete"]="true";check(rejects([&]{decodeSetupDocument(missing.dump(),empty,input);}),"typed completeness marker");
        auto duplicate=wire;duplicate["ops"].push_back(wire["ops"][0]);check(Json::parse(decodeSetupDocument(duplicate.dump(),empty,input)).at("configuration").at("rooms").size()==2,"exact repeat deduplicated without duplicate zone");
        auto excess=wire;while(excess["ops"].size()<13)excess["ops"].push_back(wire["ops"][0]);check(rejects([&]{decodeSetupDocument(excess.dump(),empty,input);}),"oversized operation list fails rather than truncating");
        check(rejects([&]{decodeSetupDocument(wire.dump(),empty,input+" Лампа не добавляй.");}),"late negative before imperative preserved");
        check(rejects([&]{decodeSetupDocument(wire.dump(),empty,input+" Не добавляй Лампа.");}),"late negative after imperative preserved");
        check(Json::parse(decodeSetupDocument(wire.dump(),empty,input+" И настрой UDP подключение.")).value("rejected",false),"unsupported tail rejects whole proposal");
        check(rejects([&]{validateSetupDocumentInput("Создай MQTT: password=private-value");}),"password rejected before model and persistence");
        check(rejects([&]{validateSetupDocumentInput("Пароль не знаю");}),"credentials discussion stays outside model document");
        check(!rejects([&]{validateSetupDocumentInput("MQTT без пароля. Пароль введу отдельно.");}),"separate credential entry is an allowed value-free instruction");
        check(rejects([&]{validateSetupDocumentInput("MQTT без пароля: private-value");}),"benign-prefix wording cannot hide an actual secret");
        check(rejects([&]{decodeSetupDocument("```json\n{}\n```",empty,input);}),"malformed response rejected");

        Json mqtt={{"ops",Json::array({Json::array({"location","Кухня"}),Json::array({"power_device","Кухня","Свет"}),
            Json::array({"connection","Брокер",{{"type","mqtt"},{"network",{{"host","127.0.0.1"},{"port",1883}}},{"mqtt",{{"tls",false}}}}}),
            Json::array({"binding","Кухня","Свет","ON","Брокер",{{"mqtt",{{"topic","home/light/set"},{"payload","ON"},{"qos",0},{"retain",false}}}}}),
            Json::array({"binding","Кухня","Свет","OFF","Брокер",{{"mqtt",{{"topic","home/light/set"},{"payload","OFF"},{"qos",0},{"retain",false}}}}}),
            Json::array({"observe","Кухня","Свет","Брокер",{{"mqtt",{{"topic","home/light/state"},{"format","text"},{"field","power"},{"true_payload","ON"},{"false_payload","OFF"},{"qos",0}}},{"fields",Json::object()}}})})},{"complete",true}};
        const std::string mqttText="Создай Кухня, Свет включать и выключать. MQTT Брокер 127.0.0.1 порт 1883, topic home/light/set, ON включает, OFF выключает. Состояние home/light/state format=text ON/OFF.";
        auto connected=Json::parse(decodeSetupDocument(mqtt.dump(),empty,mqttText));
        check(connected.at("configuration").at("connections").size()==1&&connected.at("configuration").at("bindings").size()==2&&connected.at("configuration").at("observations").size()==1,"real MQTT config schema accepts explicit semantic connection mapping observation transaction");
        check(store.current()->contentHash()==baseHash,"proposal never saves active ConfigStore");
        auto priorConnection=connected.at("configuration");
        priorConnection["connections"][0]["enabled"]=false;
        priorConnection["connections"][0]["mqtt"]["credentials_id"]="iot_credentials";
        priorConnection["bindings"]=Json::array();priorConnection["observations"]=Json::array();
        const auto priorCatalog=admin_test::catalog(priorConnection);
        const auto edit=Json::parse(decodeSetupDocument(R"({"ops":[["connection","Брокер",{"type":"mqtt","network":{"host":"127.0.0.2","port":1883}}]],"complete":true})",priorCatalog,"Измени MQTT Брокер host 127.0.0.2 port 1883"));
        check(edit.at("configuration").at("connections")[0].at("mqtt").at("credentials_id")=="iot_credentials" &&
              edit.at("configuration").at("connections")[0].at("enabled")==false,"partial connection edit preserves credentials reference and disabled state");
        auto incompleteMqtt=mqtt;incompleteMqtt["ops"].erase(incompleteMqtt["ops"].begin()+4);
        check(rejects([&]{decodeSetupDocument(incompleteMqtt.dump(),empty,mqttText+" payload ON, payload OFF.");}),"explicit MQTT OFF mapping cannot be silently omitted");
        const std::string observationOnly="Для Света в Кухня MQTT Брокер: topic home/light/newstate, format text, field power, true_payload ON, false_payload OFF.";
        auto changedObservation=mqtt["ops"][5];changedObservation[4]["mqtt"]["topic"]="home/light/newstate";
        check(!rejects([&]{decodeSetupDocument(Json{{"ops",Json::array({changedObservation})},{"complete",true}}.dump(),admin_test::catalog(connected.at("configuration")),observationOnly);}),"read-only true_payload does not require an invented write command");
        const Json flatMqtt={{"ops",Json::array({mqtt["ops"][0],mqtt["ops"][1],
            Json::array({"mqtt_connection","Брокер","127.0.0.1",1883,false,nullptr}),
            Json::array({"mqtt_binding","Кухня","Свет","ON","Брокер","home/light/set","ON",0,false}),
            Json::array({"mqtt_binding","Кухня","Свет","OFF","Брокер","home/light/set","OFF",0,false}),
            Json::array({"mqtt_observe","Кухня","Свет","Брокер","home/light/state","power","ON","OFF",0})})},{"complete",true}};
        check(Json::parse(decodeSetupDocument(flatMqtt.dump(),empty,mqttText)).at("configuration")==connected.at("configuration"),"scalar MQTT wire uses identical original-fact and configuration validation");
        check(rejects([&]{decodeSetupDocument(R"({"ops":[["location","Кухня"],["power_device","Кухня","Свет"]],"complete":true})",empty,"Создай Кухня и Свет включать и выключать, подключи по MQTT, адрес ещё не сообщил");}),"missing connection facts cannot produce a complete semantic-only prefix");
        auto invented=mqtt;invented["ops"][2][2]["network"]["host"]="127.0.0.2";
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,mqttText);}),"invented host rejected");
        invented=mqtt;invented["ops"][2][2]["network"]["port"]=188;
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,mqttText);}),"port cannot match a substring of another number");
        invented=mqtt;invented["ops"][3][5]["mqtt"]["payload"]="TOGGLE";
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,mqttText);}),"invented payload rejected");
        invented=mqtt;invented["ops"][5][4]["mqtt"]["topic"]="home/other/state";
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,mqttText);}),"invented observation topic rejected");
        invented=mqtt;invented["ops"][2][2]["mqtt"]["password"]="private-value";
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,mqttText);}),"model cannot place raw secret in config");
        check(rejects([&]{decodeSetupDocument(mqtt.dump(),empty,mqttText+" Но без подключений.");}),"late global no-connect restriction wins over model");
        invented=mqtt;invented["ops"][3][5]["mqtt"]["retain"]=true;
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,mqttText);}),"retain true not inferred");
        check(rejects([&]{setup_document::facts(Json{{"mqtt",{{"tls",true}}}},Json(),"MQTT TLS false; retain true");}),"retain true cannot authorize TLS true");
        check(rejects([&]{setup_document::facts(Json{{"mqtt",{{"retain",true}}}},Json(),"MQTT TLS true; retain false");}),"TLS true cannot authorize retained publishing");
        check(rejects([&]{setup_document::facts(Json{{"mqtt",{{"tls",true}}}},Json{{"mqtt",{{"tls",false}}}},"MQTT TLS false");}),"existing TLS false cannot be silently changed");
        check(!rejects([&]{setup_document::facts(Json{{"mqtt",{{"tls",true},{"retain",true}}}},Json(),"MQTT TLS true; retain true");}),"explicit TLS and retain opt-ins remain supported");
        const auto connectedCatalog=admin_test::catalog(connected.at("configuration"));
        auto keepOn=Json{{"ops",Json::array({mqtt["ops"][3]})},{"complete",true}};
        keepOn["ops"][0][5]["mqtt"]["qos"]=1;
        check(!rejects([&]{decodeSetupDocument(keepOn.dump(),connectedCatalog,"Для Света в Кухня измени только QoS включения через MQTT Брокер на 1");}),"selected ON mapping can reuse its own unchanged facts");
        auto wrongOn=keepOn;wrongOn["ops"][0][5]["mqtt"]["payload"]="OFF";
        check(rejects([&]{decodeSetupDocument(wrongOn.dump(),connectedCatalog,"Для Света в Кухня измени только QoS включения через MQTT Брокер на 1");}),"selected ON mapping cannot borrow another action's OFF payload");

        Json http={{"ops",Json::array({Json::array({"location","Кухня"}),Json::array({"power_device","Кухня","Свет"}),
            Json::array({"connection","HTTP стенд",{{"type","http"},{"http",{{"base_url","http://127.0.0.1:9001"},{"base_path",""}}}}}),
            Json::array({"binding","Кухня","Свет","ON","HTTP стенд",{{"http",{{"method","POST"},{"path","/set"},{"body",{{"power",true}}},{"expected_status",200}}}}}),
            Json::array({"observe","Кухня","Свет","HTTP стенд",{{"http",{{"method","GET"},{"path","/state"},{"expected_status",200}}},{"fields",{{"power","power"}}}}})})},{"complete",true}};
        const std::string httpText="Создай Кухня Свет включать и выключать. HTTP стенд http://127.0.0.1:9001 POST /set body {\"power\":true}, GET /state поле power.";
        auto httpResult=Json::parse(decodeSetupDocument(http.dump(),empty,httpText));
        check(httpResult.at("configuration").at("bindings").size()==1&&httpResult.at("configuration").at("observations").size()==1,"explicit HTTP creation and observed state config");
        invented=http;invented["ops"][3][5]["http"]["path"]="/factory-reset";check(rejects([&]{decodeSetupDocument(invented.dump(),empty,httpText);}),"invented HTTP path rejected");
        invented=http;invented["ops"][3][5]["http"]["body"]["power"]=false;check(rejects([&]{decodeSetupDocument(invented.dump(),empty,httpText);}),"invented HTTP body value rejected");
        Json modbus={{"ops",Json::array({Json::array({"location","Стенд"}),Json::array({"power_device","Стенд","Реле"}),
            Json::array({"connection","Контроллер",{{"type","modbus_tcp"},{"network",{{"host","127.0.0.1"},{"port",1502}}},{"modbus",{{"unit_id",1}}}}}),
            Json::array({"binding","Стенд","Реле","ON","Контроллер",{{"modbus",{{"function",5},{"address",10},{"data_type","boolean"},{"value",true}}}}}),
            Json::array({"observe","Стенд","Реле","Контроллер",{{"modbus",{{"function",1},{"address",11},{"field","power"},{"data_type","boolean"}}},{"fields",Json::object()}}})})},{"complete",true}};
        const std::string modbusText="Создай Стенд и Реле включать и выключать. Контроллер Modbus TCP 127.0.0.1 порт 1502 unit_id 1. ON function 5 address 10 data_type boolean value true; состояние function 1 address 11 поле power boolean.";
        auto modbusResult=Json::parse(decodeSetupDocument(modbus.dump(),empty,modbusText));
        check(modbusResult.at("configuration").at("connections")[0].at("type")=="modbus_tcp"&&modbusResult.at("configuration").at("bindings").size()==1,"explicit Modbus TCP plan admitted by real config schema");
        invented=modbus;invented["ops"][3][5]["modbus"]["address"]=12;
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,modbusText);}),"invented Modbus address rejected");
        invented=modbus;invented["ops"][3][5]["modbus"]["address"]=1;
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,modbusText);}),"Unit ID or function number cannot stand in for labelled register address");
        invented=modbus;invented["ops"][2][2]["modbus"]["unit_id"]=2;
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,modbusText);}),"invented Modbus unit rejected");
        auto rtu=modbus;rtu["ops"][2][2]={{"type","modbus_rtu"},{"serial",{{"port","/dev/ttyUSB0"},{"baud_rate",9600},{"parity","even"},{"stop_bits",1}}},{"modbus",{{"unit_id",1}}}};
        const std::string rtuText="Создай Стенд и Реле включать и выключать. Контроллер Modbus RTU /dev/ttyUSB0 baud_rate 9600 parity even stop_bits 1 unit_id 1. ON function 5 address 10 data_type boolean value true; состояние function 1 address 11 поле power boolean.";
        check(Json::parse(decodeSetupDocument(rtu.dump(),empty,rtuText)).at("configuration").at("connections")[0].at("type")=="modbus_rtu","explicit Modbus RTU draft without opening serial device");
        invented=rtu;invented["ops"][2][2]["serial"]["port"]="/dev/ttyUSB1";
        check(rejects([&]{decodeSetupDocument(invented.dump(),empty,rtuText);}),"invented serial path rejected");
        const std::string perDevice="Создай Кухня, Свет включать и выключать. Добавь Телевизор в Кухня, без управления.";
        const Json perWire={{"ops",Json::array({Json::array({"location","Кухня"}),Json::array({"power_device","Кухня","Свет"}),Json::array({"power_device","Кухня","Телевизор"})})}};
        const auto perResult=Json::parse(decodeSetupDocument(perWire.dump(),empty,perDevice));
        check(perResult.at("configuration").at("devices")[0].at("action_ids").size()==2&&perResult.at("configuration").at("devices")[1].at("action_ids").empty(),"model cannot copy power capabilities from lamp to explicitly unconfigured TV");
        auto semantic=admin_test::catalog(result.at("configuration"));
        check(rejects([&]{decodeSetupDocument(R"({"ops":[["delete_device","Кухня","Свет"]]})",semantic,"Расскажи какие устройства настроены");}),"unrequested model deletion rejected");
        check(rejects([&]{decodeSetupDocument(R"({"ops":[["rename_device","Кухня","Свет","Новый"]]})",semantic,"Переименуй его в Новый");}),"model cannot select referent without unambiguous focus");
        // The generic old Copilot API cannot bypass original-document grounding.
        check(rejects([&]{decodeCopilotProposal(Json{{"status","ok"},{"summary","test"},{"operations",Json::array({{{"op","setup_connection"},{"name","x"},{"settings",{{"type","mqtt"}}}}})}}.dump(),empty);}),"document-only transport operations unavailable to legacy decoder");
        check(store.current()->contentHash()==baseHash,"all forced negative outputs leave real store unchanged");
    } catch(const std::exception& error) {++failures;std::cerr<<"EXCEPTION "<<error.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
