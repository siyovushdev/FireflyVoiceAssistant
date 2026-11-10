#include "Support.h"
#include "AdminSetupPlanning.h"
#include "SetupSession.h"
#include <iostream>

using namespace firefly::admin;
namespace config=firefly::config;
namespace {
Json read(const std::string& path) {std::ifstream file(path);return Json::parse(file);}
Json evaluate(const Json& request,config::Catalog empty) {
    const auto catalog=request.contains("catalog")?admin_test::catalog(request.at("catalog")):empty;
    try {
        const auto input=request.at("text").get<std::string>();
        if(request.value("mode",std::string())=="timeout")return Json{{"timeout",setupConnectionTimeout(input)}};
        if(request.value("mode",std::string())!="staged")return Json::parse(decodeSetupDocument(request.at("response").dump(),catalog,input));
        const auto ops=request.at("response").at("ops");int stage=0;Json list=Json::array();
        for(const auto& op:ops)list.push_back(Json::array({op[1],op[2],op[4].is_string()?Json("mqtt"):op[4][0]}));
        return planSetupDocument(catalog,input,Json::array(),"",[&](const std::string&,std::string_view original,int,const auto& decode,int) {
            if(original!=input)throw std::runtime_error("Original request changed");
            const auto raw=stage==0?Json{{"devices",list}}:Json{{"connection",ops.at(stage-1)[4]},{"commands",ops.at(stage-1)[5]},{"observation",ops.at(stage-1)[6]}};
            ++stage;
            try{return Json{{"ok",true},{"suggestion",Json::parse(decode(raw.dump()))},{"retry_count",0},{"trace",Json::array()}};}
            catch(const std::exception& e){return Json{{"ok",false},{"error",e.what()},{"retry_count",0},{"trace",Json::array()}};}
        });
    } catch(const std::exception& e) {return Json{{"error",e.what()}};}
}
bool candidate(const Json& result) {
    const auto& suggestion=result.contains("suggestion")?result.at("suggestion"):result;
    return suggestion.contains("configuration");
}
struct SourceModel : admin_test::Model {
    Json ops,correctOps,fullResponse;int calls=0;std::map<std::string,int> attempts;bool admin=false,repairMalformed=false;
    bool setupRequired() const override{return true;}
    void enterSetup() override{admin=true;}
    void leaveSetup(const std::string& value) override{admin=false;useContext(value);}
    void useAdminContext(const std::string& value,int) override{prompt=value;}
    void useAdminDocumentContext(const std::string& value,int) override{prompt=value;}
    std::string inferAdmin(std::string_view) override {
        ++calls;
        if(prompt.find("First list ONLY")!=std::string::npos) {
            Json inventory=Json::array();for(const auto& op:ops)inventory.push_back(Json::array({op[1],op[2],op[4][0]}));
            return Json{{"devices",inventory}}.dump();
        }
        const auto start=prompt.rfind("\nTARGET=");
        if(start!=std::string::npos) {
            const auto end=prompt.find('\n',start+8);const auto target=Json::parse(prompt.substr(start+8,end-start-8));
            const int count=++attempts[target.dump()];
            if(repairMalformed&&target[1]=="Торшер"&&count==1)return "{broken";
            const auto& choices=count>1&&!correctOps.is_null()?correctOps:ops;
            for(const auto& op:choices)if(op[1]==target[0]&&op[2]==target[1])
                return Json{{"connection",op[4]},{"commands",op[5]},{"observation",op[6]}}.dump();
        }
        if(prompt.find("You configure Firefly")!=std::string::npos||prompt.find("You plan Firefly")!=std::string::npos)return fullResponse.dump();
        return "{}"; // Optional language proposals are not this test's inference seam.
    }
};
}
int main(int argc,char** argv) {
    int checks=0,failures=0;
    const auto check=[&](bool condition,const std::string& name) {++checks;if(!condition){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    try {
        if(argc!=2)throw std::runtime_error("source root required");const std::string root=argv[1];
        const auto empty=config::ConfigLoader::fromFile(root+"/config/empty-home.json");
        for(const std::string list:{"MQTT, HTTP", "HTTP, MQTT", "MQTT, HTTP и Modbus TCP", "HTTP, Modbus TCP, MQTT"}) {
            for(const std::string input:{list+" не используй", "Не используй "+list}) {
                const auto parsed=setup_source::transports(input);
                check(parsed.first.empty()&&parsed.second.count("mqtt")&&parsed.second.count("http"),"shared negative predicate: "+input);
            }
        }
        for(const std::string input:{"Используй MQTT, а не HTTP", "MQTT используй, HTTP не используй", "Не используй HTTP, используй MQTT", "Через MQTT, не HTTP"}) {
            const auto parsed=setup_source::transports(input);
            check(parsed.first==std::set<std::string>{"mqtt"}&&parsed.second==std::set<std::string>{"http"},"separate predicates: "+input);
        }
        for(const std::string list:{"MQTT, HTTP", "HTTP, MQTT"}) {
            for(const std::string input:{"Не используй "+list+", а Modbus TCP используй", "Используй Modbus TCP, но не используй "+list}) {
                const auto parsed=setup_source::transports(input);
                check(parsed.first==std::set<std::string>{"modbus_tcp"}&&parsed.second==std::set<std::string>{"mqtt","http"},"compound list predicate scope: "+input);
            }
        }
        // The same raw token must have the same numeric meaning in every
        // supported notation. Prose punctuation trimming must never touch it.
        for(const std::string value:{"-1","-0","+1","1.5","1,5","1e0",""," ","- 1","null","false","[]","{}","0","1"}) {
            for(const std::string input:{"QoS: "+value,"QoS независимого состояния MQTT на "+value,"{\"qos\":"+value+"}","{\"qos\":"+Json(value).dump()+"}"}) {
                bool valid=false;std::vector<int> numbers;
                try {numbers=setup_source::qosNumbers(input);valid=!numbers.empty();}catch(const std::exception&){}
                const bool expected=value=="0"||value=="1";
                check(valid==expected&&(!valid||(numbers.size()==1&&numbers[0]==std::stoi(value))),"numeric token preserved across notations: "+input);
            }
        }
        Json control;
        for(const auto& file:std::filesystem::directory_iterator(root+"/tests/admin/setup-review-cases")) {
            const auto name=file.path().stem().string();const auto request=read(file.path());const auto result=evaluate(request,empty);
            const std::set<std::string> positives={"00-control","06-one-line-valid","07-same-name-control","22-negated-device-correct-plan","24-newline-timeout","27-negated-transport-correct-plan","28-mqtt-inverse-readback-correct","29-read-function-next-line","33-http-active-low-control","34-modbus-active-low-control","35-matching-reuse-credentials-control","38-retry-malformed-control","48-inline-transport-prohibition","51-quoted-state-payload-control","52-conjoined-transport-prohibition","56-observation-correct-broker-control","58-modbus-read-only-control","61-quoted-payload-field-control","62-comma-transport-control","63-integer-http-url-control","69-escaped-state-payload-control","70-unicode-state-payload-control","72-reversed-transport-control","77-http-uri-path-control","78-named-observation-control","80-named-observation-explicit-host-control","82-repeated-observation-control","103-mixed-predicates-control","104-bare-contrast-control","108-other-read-list-control","113-qos-explicit-matching-host-control","120-quoted-negation-control","121-quoted-role-field-control","84-separate-predicates-control","85-quoted-command-words","86-quoted-adjectives-control","87-quoted-field-names","88-quoted-uri-values","89-quoted-delimiters-control","92-forbidden-other-address-control","93-positive-negative-comparison","97-qos-only-preserve-endpoint","98-qos-only-explicit-reference-control","122-quoted-syntax-is-data-control","123-guillemet-uri-control","124-quoted-http-endpoint-control","126-quoted-mapping-keys","127-json-mapping-object","128-bare-mapping-keys-control","131-lowercase-quoted-http-control","133-qos-requested-value-control","135-prefix-list-followed-by-positive","136-split-sentence-control","138-suffix-list-followed-by-positive-control","139-qos-quoted-unrelated-data-control","151-json-qos-control","154-compound-list-order-control","155-compound-list-leading-control","158-json-key-like-values-control","159-qos-in-payload-control","162-separate-command-state-qos-control","163-qos-zero-control","164-qos-repeated-equal-control","168-bare-integer-qos-control","170-json-qos-topic-control","172-repeated-equal-json-mapping-control","175-command-qos-after-state","176-command-marker-after-state-control","178-structured-opaque-values-control","179-compound-reversed-control","182-isolated-bare-integer-control","184-command-qos-before-state-control","191-qos-equal-json-repeat-control","196-bare-whitespace-integer-control","197-mention-then-newline-field-control","198-mention-then-json-control","201-command-qos-colon-control","202-command-qos-short-role-control"};
            const bool positive=positives.count(name);
            if(name=="10-timeout-unitless")check(result.value("timeout",0)==1000,name);
            else if(name=="11-timeout-range")check(result.value("timeout",0)==999999,name+" schema, not parser, owns upper range");
            else check(candidate(result)==positive,name+" "+result.dump().substr(0,220));
            if(name=="24-newline-timeout"&&candidate(result))
                for(const auto& c:result.at("suggestion").at("configuration").at("connections"))if(c.at("name")=="Вентилятор")check(c.at("timeout_ms")==1000,"multiline timeout is preserved, not defaulted");
            if(name=="00-control")control=request;
            if(name=="07-same-name-control"&&candidate(result)) {
                const auto& c=result.at("suggestion").at("configuration");
                check(c.at("connections").size()==2,"same device names have distinct connections");
                std::set<std::string> hosts,names;
                for(const auto& connection:c.at("connections")) {hosts.insert(connection.at("network").at("host"));names.insert(connection.at("name"));}
                check(hosts==std::set<std::string>{"192.168.10.11","192.168.10.22"}&&names.size()==2,"zone identity preserves both endpoints");
            }
        }
        // The same broker reference is valid when its endpoint actually matches.
        auto reuse=read(root+"/tests/admin/setup-review-cases/19-reuse-wrong-endpoint-staged.json");
        reuse["catalog"]["connections"][0]["network"]["host"]="192.168.10.11";
        check(candidate(evaluate(reuse,empty)),"matching existing endpoint can be reused with settings intact");
        auto tls=reuse;tls["text"]=tls.at("text").get<std::string>()+" TLS true.";
        // Place an explicit TLS requirement in the actual device section.
        auto tlsText=reuse.at("text").get<std::string>();tlsText.insert(tlsText.find("topic house/set"),"TLS true, ");tls["text"]=tlsText;
        check(!candidate(evaluate(tls,empty)),"existing credentials/name cannot override explicitly required TLS");
        auto protocol=reuse;protocol["text"]=std::regex_replace(protocol.at("text").get<std::string>(),std::regex("MQTT"),"HTTP");
        check(!candidate(evaluate(protocol,empty)),"existing connection must also match the requested transport");
        auto httpCatalog=Json::parse(empty->canonicalJson());
        httpCatalog["connections"]=Json::array({{{"id","old_web"},{"name","Web"},{"type","http"},{"enabled",true},{"timeout_ms",2000},{"http",{{"base_url","http://192.168.10.11:200"},{"base_path",""}}}}});
        const auto httpRequest=Json{{"catalog",httpCatalog},{"text","Создай зону Кабинет. Добавь устройство Лампа, включать и выключать. Подключение HTTP base_url http://192.168.10.11:8080. Включение: POST /set с JSON {\"on\":true}. Выключение: POST /set с JSON {\"on\":false}. expected_status 200."},
            {"response",{{"complete",true},{"ops",Json::array({{{"op","power_setup"},{"zone","Кабинет"},{"device","Лампа"},{"connection","Web"},{"commands",Json::array({"http","POST","/set",Json{{"on",true}},"/set",Json{{"on",false}},200})},{"observation",nullptr}}})}}}};
        check(!candidate(evaluate(httpRequest,empty)),"HTTP status 200 cannot authorize endpoint port 200 instead of requested URL port");
        auto matchingHttp=httpRequest;matchingHttp["catalog"]["connections"][0]["http"]["base_url"]="http://192.168.10.11:8080";
        check(candidate(evaluate(matchingHttp,empty)),"full matching HTTP URL existing-reference positive control");
        auto payloadHost=control;auto payloadText=payloadHost.at("text").get<std::string>();
        payloadText.insert(payloadText.find("- топик команд:"),"Диагностические данные JSON {\"host\":\"192.168.68.199\"}. ");payloadHost["text"]=payloadText;
        check(candidate(evaluate(payloadHost,empty)),"a JSON data host is not a connection declaration");
        payloadHost["response"]["ops"][0][4][1]="192.168.68.199";
        check(!candidate(evaluate(payloadHost,empty)),"host from JSON data cannot replace the explicit broker host");
        // Active-low or arbitrary wire values are not forbidden: source intent,
        // rather than a hardcoded ON=true or ON=payload ON rule, is authoritative.
        auto inverse=control;auto text=inverse.at("text").get<std::string>();
        const auto replace=[&](const std::string& from,const std::string& to) {const auto at=text.find(from);if(at==std::string::npos)throw std::runtime_error("fixture phrase missing");text.replace(at,from.size(),to);};
        replace("включение — точный текст ON","включение — точный текст OFF");
        replace("выключение — точный текст OFF","выключение — точный текст ON");inverse["text"]=text;
        inverse["response"]["ops"][0][5][2]="OFF";inverse["response"]["ops"][0][5][4]="ON";
        check(candidate(evaluate(inverse,empty)),"explicit inverse wire mapping has a positive control");
        inverse["response"]["ops"][0][5][2]="ON";inverse["response"]["ops"][0][5][4]="OFF";
        check(!candidate(evaluate(inverse,empty)),"readback ON/OFF values cannot authorize a different write mapping");
        auto keyed=control;auto keyedText=keyed.at("text").get<std::string>();
        const auto bodyAt=keyedText.find("{\"on\":true}");keyedText.replace(bodyAt,std::string("{\"on\":true}").size(),"{\"on\":true,\"dry_run\":false}");keyed["text"]=keyedText;
        keyed["response"]["ops"][1][5][3]={{"on",true},{"dry_run",false}};
        check(candidate(evaluate(keyed,empty)),"multi-field HTTP body positive control");
        keyed["response"]["ops"][1][5][3]["on"]=false;
        check(!candidate(evaluate(keyed,empty)),"another HTTP field's false cannot authorize ON inversion");
        auto wordBody=control;auto wordText=wordBody.at("text").get<std::string>();
        const auto wordAt=wordText.find("{\"on\":true}");wordText.replace(wordAt,std::string("{\"on\":true}").size(),"{\"state\":\"выключено\"}");wordBody["text"]=wordText;
        wordBody["response"]["ops"][1][5][3]={{"state","выключено"}};
        check(candidate(evaluate(wordBody,empty)),"Russian words inside HTTP JSON values do not become action labels");
        auto unrelated=control;auto unrelatedText=unrelated.at("text").get<std::string>();
        const auto valueAt=unrelatedText.find("включение — true");unrelatedText.replace(valueAt,std::string("включение — true").size(),"включение — true (не false)");unrelated["text"]=unrelatedText;
        unrelated["response"]["ops"][2][5][4]=false;
        check(!candidate(evaluate(unrelated,empty)),"negated or unrelated scalar inside ON clause is not its Modbus value");
        for(const std::string invalid:{"timeout 1000 ms; timeout -5 ms","timeout 1000 ms; timeout nope","timeout 1000 ms; timeout 1.5 ms"})
            check(evaluate(Json{{"mode","timeout"},{"text",invalid}},empty).contains("error"),"every timeout declaration must parse: "+invalid);
        check(evaluate(Json{{"mode","timeout"},{"text","Включение: POST /set JSON {\"timeout\":1000}"}},empty).contains("error"),"JSON command data cannot silently set the connection timeout");
        check(setupConnectionTimeout("MQTT topic house/timeout/set; автоматизацию по таймеру не добавляй")==2000,"topic bytes and timer wording are not timeout declarations");
        for(const std::string sessionCase:{"00-control","01-inverted-mqtt","02-inverted-http","03-inverted-modbus","04-missing-modbus-readback","05-omitted-rtu-device","13-timeout-contradiction-full","20-modbus-write-wrong-address","21-mqtt-readback-inverted","23-fractional-port","24-newline-timeout","27-negated-transport-correct-plan","30-empty-timeout-field","31-valid-plus-empty-timeout","32-retry-repairable-inversion","38-retry-malformed-control","39-retry-repairable-readback","40-retry-repairable-coil","49-fractional-http-url-port","50-port-from-command-payload","51-quoted-state-payload-control","52-conjoined-transport-prohibition","53-fractional-unit-control","54-blank-line-timeout-control","55-port-suffix-control","56-observation-correct-broker-control","57-observation-wrong-broker","58-modbus-read-only-control","59-modbus-read-only-wrong-address","60-observation-wrong-modbus-endpoint","61-quoted-payload-field-control","62-comma-transport-control","63-integer-http-url-control","64-fractional-observation-address","65-observation-wrong-port","66-observation-wrong-unit","67-observation-forbidden-zero","68-observation-comma-fraction","69-escaped-state-payload-control","70-unicode-state-payload-control","71-escaped-state-payload-inverted","72-reversed-transport-control","73-forbidden-transport-list","74-http-comma-fraction","75-http-exponent-port","76-http-empty-port","77-http-uri-path-control","78-named-observation-control","79-named-observation-wrong-host","80-named-observation-explicit-host-control","81-first-observation-wrong-address","82-repeated-observation-control","100-three-forbidden-read-addresses","101-prefix-transport-list","102-suffix-three-comma-list","103-mixed-predicates-control","104-bare-contrast-control","105-contrast-forbidden-mqtt","106-prefix-list-forbidden-mqtt","107-prefix-read-list","108-other-read-list-control","109-comma-read-list","110-qos-explicit-wrong-host","111-qos-explicit-wrong-port","112-qos-forbidden-mqtt","113-qos-explicit-matching-host-control","114-qos-cannot-change-topic","115-qos-cannot-invert-mapping","116-qos-cannot-switch-connection","117-quoted-readback-inverted-85","118-quoted-readback-inverted-87","119-quoted-readback-inverted-88","120-quoted-negation-control","121-quoted-role-field-control","83-comma-forbidden-transports","84-separate-predicates-control","85-quoted-command-words","86-quoted-adjectives-control","87-quoted-field-names","88-quoted-uri-values","89-quoted-delimiters-control","90-json-string-escapes-control","91-comma-forbidden-read-addresses","92-forbidden-other-address-control","93-positive-negative-comparison","94-invalid-url-port","95-invalid-url-port","96-invalid-url-port","97-qos-only-preserve-endpoint","98-qos-only-explicit-reference-control","99-three-forbidden-transports","122-quoted-syntax-is-data-control","123-guillemet-uri-control","124-quoted-http-endpoint-control","125-quoted-http-fractional-port","126-quoted-mapping-keys","127-json-mapping-object","128-bare-mapping-keys-control","129-uppercase-quoted-http-uri","130-uppercase-bare-http-uri","131-lowercase-quoted-http-control","132-qos-forbidden-value","133-qos-requested-value-control","134-qos-unstated-value-control","135-prefix-list-followed-by-positive","136-split-sentence-control","137-last-member-forbidden-control","138-suffix-list-followed-by-positive-control","139-qos-quoted-unrelated-data-control","140-quoted-invalid-http-port","141-quoted-invalid-http-port","142-quoted-invalid-http-port","143-fractional-qos","144-qos-explicit-forbidden-zero","145-qos-comma-fraction","146-qos-exponent","147-qos-negative","148-qos-empty","149-qos-request-cannot-noop","150-json-qos-fraction","151-json-qos-control","152-json-mapping-inversion","153-quoted-json-data-not-keys","154-compound-list-order-control","155-compound-list-leading-control","156-conflicting-json-mapping","157-nested-example-not-mapping","158-json-key-like-values-control","159-qos-in-payload-control","160-creation-qos-fraction","161-creation-qos-mismatch","162-separate-command-state-qos-control","163-qos-zero-control","164-qos-repeated-equal-control","165-bare-negative-qos","166-json-negative-qos","167-positive-sign-qos-control","168-bare-integer-qos-control","169-empty-json-qos","170-json-qos-topic-control","171-empty-conflicting-json-mapping","172-repeated-equal-json-mapping-control","173-empty-conflicting-json-qos","174-null-json-qos-control","175-command-qos-after-state","176-command-marker-after-state-control","177-negative-zero-qos","178-structured-opaque-values-control","179-compound-reversed-control","180-compound-forbidden-selected-control","181-isolated-bare-negative-qos","182-isolated-bare-integer-control","183-isolated-negative-zero-qos","184-command-qos-before-state-control","185-qos-empty-first-conflict","186-qos-space-value","187-qos-negative-string","188-qos-negative-spaced-string","189-qos-malformed-json-sign","190-qos-malformed-json-fraction","191-qos-equal-json-repeat-control","192-mapping-empty-first-conflict","193-bare-negative-spaced","194-bare-positive-sign","195-bare-empty-value","196-bare-whitespace-integer-control","197-mention-then-newline-field-control","198-mention-then-json-control","199-command-qos-after-state-mismatch","200-state-qos-after-command-mismatch","201-command-qos-colon-control","202-command-qos-short-role-control","203-false-mapping-empty-conflict","204-quoted-key-empty-qos"}) {
            const std::set<std::string> sessionGood={"00-control","24-newline-timeout","27-negated-transport-correct-plan","32-retry-repairable-inversion","38-retry-malformed-control","39-retry-repairable-readback","40-retry-repairable-coil","51-quoted-state-payload-control","52-conjoined-transport-prohibition","56-observation-correct-broker-control","58-modbus-read-only-control","61-quoted-payload-field-control","62-comma-transport-control","63-integer-http-url-control","69-escaped-state-payload-control","70-unicode-state-payload-control","72-reversed-transport-control","77-http-uri-path-control","78-named-observation-control","80-named-observation-explicit-host-control","82-repeated-observation-control","103-mixed-predicates-control","104-bare-contrast-control","108-other-read-list-control","113-qos-explicit-matching-host-control","120-quoted-negation-control","121-quoted-role-field-control","84-separate-predicates-control","85-quoted-command-words","86-quoted-adjectives-control","87-quoted-field-names","88-quoted-uri-values","89-quoted-delimiters-control","92-forbidden-other-address-control","93-positive-negative-comparison","97-qos-only-preserve-endpoint","98-qos-only-explicit-reference-control","122-quoted-syntax-is-data-control","123-guillemet-uri-control","124-quoted-http-endpoint-control","126-quoted-mapping-keys","127-json-mapping-object","128-bare-mapping-keys-control","131-lowercase-quoted-http-control","133-qos-requested-value-control","135-prefix-list-followed-by-positive","136-split-sentence-control","138-suffix-list-followed-by-positive-control","139-qos-quoted-unrelated-data-control","151-json-qos-control","154-compound-list-order-control","155-compound-list-leading-control","158-json-key-like-values-control","159-qos-in-payload-control","162-separate-command-state-qos-control","163-qos-zero-control","164-qos-repeated-equal-control","168-bare-integer-qos-control","170-json-qos-topic-control","172-repeated-equal-json-mapping-control","175-command-qos-after-state","176-command-marker-after-state-control","178-structured-opaque-values-control","179-compound-reversed-control","182-isolated-bare-integer-control","184-command-qos-before-state-control","191-qos-equal-json-repeat-control","196-bare-whitespace-integer-control","197-mention-then-newline-field-control","198-mention-then-json-control","201-command-qos-colon-control","202-command-qos-short-role-control"};
            const bool bad=!sessionGood.count(sessionCase);
            const auto request=read(root+"/tests/admin/setup-review-cases/"+sessionCase+".json");
            admin_test::Directory directory;config::ConfigStore store(directory.path);const auto base=store.initialize(request.contains("catalog")?admin_test::catalog(request.at("catalog")):empty);
            SourceModel model;model.ops=request.at("response").at("ops");model.fullResponse=request.at("response");
            if(request.contains("correct_response"))model.correctOps=request.at("correct_response").at("ops");
            model.repairMalformed=request.value("repair_malformed",false);
            RuntimeWorker worker(model);int publications=0;Json dialogue;
            const auto call=[&](const Json& request) {
                // Only runtime publication is stubbed. No endpoint, subscriber,
                // NPU or hardware executor is started by temporary-store Apply.
                if(request.at("method")=="apply"&&!request.at("configuration").at("devices").empty()) {++publications;return Json{{"ok",true}};}
                auto response=Json::parse(worker.handle(request.dump()));if(request.at("method")=="dialogue")dialogue=response;return response;
            };
            // The active runtime stays empty; SetupSession passes the actual store catalog
            // to dialogue/preflight, without starting real existing subscriptions.
            call(Json{{"method","apply"},{"configuration",Json::parse(empty->canonicalJson())}});SetupSession session(store,call);
            const auto start=session.handle("start",{});Json state;bool rejected=false;
            try {state=session.handle("message",{{"session_id",start.at("session_id")},{"version",0},{"request_id","source-contract"},{"text",request.at("text")},{"atomic_message",true}});}
            catch(const std::exception&){rejected=true;}
            check(store.current()->contentHash()==base->contentHash()&&Json::parse(worker.observe()).at("executions").empty()&&publications==0,"private planning never saves or executes");
            if(bad)check(rejected,sessionCase+" cannot enter an accepted SetupSession draft");
            else {
                check(!rejected&&state.at("configuration").at("devices").size()==4,"full RuntimeWorker/SetupSession positive control "+sessionCase);
                if(request.contains("correct_response")||model.repairMalformed) {
                    const auto retried=std::count_if(model.attempts.begin(),model.attempts.end(),[](const auto& x){return x.second==2;});
                    check(retried==1&&dialogue.at("retry_count")==1,"repairable stage calls inference twice and consumes exactly one shared retry");
                    check(std::none_of(dialogue.at("trace").begin(),dialogue.at("trace").end(),[](const Json& x){return x.value("stage",std::string())=="combined_validation";}),"repair occurs before combined validation");
                }
                if(!rejected) {
                    const auto applied=session.handle("apply",{{"session_id",start.at("session_id")},{"version",state.at("version")}});
                    check(applied.at("status")=="applied"&&store.current()->configuration().devices.size()==4&&publications==1,"explicit isolated Apply persists valid source-aligned config");
                    if(request.contains("catalog")) {
                        const auto saved=Json::parse(store.current()->canonicalJson());
                        const auto expected=Json::parse(decodeSetupDocument(request.at("response").dump(),base,request.at("text").get<std::string>())).at("configuration");
                        // Apply increments the CAS revision and confirms reviewed
                        // entity metadata. Compare the transport contracts exactly.
                        check(saved.at("observations")==expected.at("observations")&&saved.at("connections")==expected.at("connections")&&saved.at("bindings")==expected.at("bindings"),"read-only Apply preserves validated readback/endpoints and command bindings exactly");
                        check(saved.at("revision").get<int>()==Json::parse(base->canonicalJson()).at("revision").get<int>()+1,"read-only Apply advances CAS revision exactly once");
                    }
                }
            }
        }
    } catch(const std::exception& e) {++failures;std::cerr<<"EXCEPTION "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
