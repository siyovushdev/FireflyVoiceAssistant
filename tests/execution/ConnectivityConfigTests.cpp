#include "../admin/Support.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/config/Connectivity.h"
#include <iostream>

using namespace admin_test;
using namespace firefly;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } check(rejected, "expected strict rejection"); }
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        auto old = config::ConfigValidator::validate(config::migrateV2(catalog(base(argv[1]))->configuration()));
        auto value = Json::parse(old->canonicalJson());
        // New optional empty collections do not alter old canonical hashes.
        value["connections"] = Json::array(); value["observations"] = Json::array();
        check(catalog(value)->contentHash() == old->contentHash(), "old persisted hashes preserved");
        Json c{{"id","connection_http"},{"name","Controller"},{"type","http"},{"enabled",true},{"timeout_ms",2000},
            {"http",{{"base_url","http://127.0.0.1:8765"},{"base_path","/api"}}}};
        Json binding{{"target_id","device_kitchen_light"},{"action_id","action_set_brightness"},{"connection_id","connection_http"},{"executor","http"},
            {"http",{{"method","POST"},{"path","/level"},{"argument","brightness"},{"body",Json::object()},{"expected_status",204}}}};
        Json observation{{"target_id","device_kitchen_light"},{"connection_id","connection_http"},{"executor","http"},
            {"http",{{"method","GET"},{"path","/level"},{"expected_status",200}}},{"fields",{{"level","brightness"}}}};
        Json proposal{{"status","ok"},{"summary","HTTP mapping"},{"operations",Json::array({
            {{"op","set_http_connection"},{"connection",c}},{{"op","set_connection_binding"},{"binding",binding}},
            {{"op","set_http_observe"},{"observation",observation}}})}};
        const auto decoded = Json::parse(admin::decodeCopilotProposal(proposal.dump(), old));
        check(!decoded.at("saved") && !decoded.at("applied"), "AI is proposal only");
        const auto connected = catalog(decoded.at("configuration"));
        const auto roundtrip = config::ConfigLoader::fromJson(connected->canonicalJson());
        check(roundtrip->contentHash() == connected->contentHash(), "roundtrip hash");
        const auto& config = connected->configuration();
        const auto effective = config::effectiveHttp(config.bindings.front().http, &config.connections.front());
        check(effective.url == "http://127.0.0.1:8765/api/level" && effective.timeoutMs == 2000, "base path + inherited timeout");
        check(Json::parse(admin::acceptCopilotProposal(old->canonicalJson(), decoded.dump())).at("ok"), "accept only into draft");
        rejects([&] { admin::acceptCopilotProposal(connected->canonicalJson(), decoded.dump()); });
        auto bad = proposal;
        bad["operations"][1]["binding"]["action_id"] = "action_invented";
        rejects([&] { admin::decodeCopilotProposal(bad.dump(), old); });
        bad = proposal; bad["operations"][1]["execute"] = true;
        rejects([&] { admin::decodeCopilotProposal(bad.dump(), old); });
        bad = proposal; bad["operations"].push_back({{"op","rename_device"},{"device","device_kitchen_light"},{"location","Кухня"},{"name","Hacked"}});
        rejects([&] { admin::decodeCopilotProposal(bad.dump(), old); });
        // Connections/observations use the existing durable store, backup and LKG.
        Directory directory;
        config::ConfigStore store(directory.path);
        store.initialize(connected);
        store.recordLastKnownGood(connected);
        auto renamed = Json::parse(connected->canonicalJson());
        renamed["connections"][0]["name"] = "Renamed";
        const config::ExpectedRevision expected{connected->configuration().revision, connected->contentHash()};
        const auto saved = store.save(catalog(renamed), expected);
        rejects([&] { store.save(connected, expected); });
        check(store.revision(connected->configuration().revision)->configuration().observations.size() == 1, "backup retains observe");
        std::ofstream(directory.path + "/configuration.json") << "{broken";
        const auto lkg = store.lastKnownGood();
        check(lkg && (*lkg)->contentHash() == connected->contentHash(), "corrupt-current LKG recovery retains connectivity");
        check((*lkg)->configuration().connections.size() == 1 && (*lkg)->configuration().bindings.size() == 1, "LKG complete graph");
        (void)saved;
        // MQTT is an executable transport; its credentials are references only.
        auto mqttConfig = value;
        mqttConfig["connections"] = Json::array({{
            {"id","connection_mqtt"},{"name","Local broker"},{"type","mqtt"},{"enabled",true},{"timeout_ms",2000},
            {"network",{{"host","127.0.0.1"},{"port",1883}}},{"mqtt",{{"tls",false},{"credentials_id","relay_secret"}}}
        }});
        mqttConfig["bindings"] = Json::array({{
            {"target_id","device_entrance_door"},{"action_id","action_open_entrance"},
            {"connection_id","connection_mqtt"},{"executor","mqtt"},
            {"mqtt",{{"topic","home/light/set"},{"payload","40"},{"qos",0},{"retain",false}}}
        }});
        mqttConfig["observations"] = Json::array({{
            {"target_id","device_entrance_door"},{"connection_id","connection_mqtt"},{"executor","mqtt"},
            {"mqtt",{{"topic","home/door/state"},{"format","json"},{"qos",0}}},
            {"fields",{{"position","position"}}}
        }});
        const auto mqttSnapshot = catalog(mqttConfig);
        check(config::ConfigLoader::fromJson(mqttSnapshot->canonicalJson())->contentHash() == mqttSnapshot->contentHash(), "MQTT canonical roundtrip");
        auto mqttBad = mqttConfig;
        mqttBad["bindings"][0]["mqtt"]["topic"] = "home/+/set";
        rejects([&] { catalog(mqttBad); });
        mqttBad = mqttConfig;
        mqttBad["bindings"][0]["target_id"] = "device_kitchen_light";
        mqttBad["bindings"][0]["action_id"] = "action_set_brightness";
        rejects([&] { catalog(mqttBad); }); // fixed MQTT payload cannot silently ignore brightness
        mqttBad = mqttConfig; mqttBad["connections"][0]["mqtt"]["credentials_id"] = "../secrets";
        rejects([&] { catalog(mqttBad); });
        mqttBad = mqttConfig; mqttBad["bindings"][0]["executor"] = "http";
        rejects([&] { catalog(mqttBad); });
        mqttBad = mqttConfig; mqttBad["observations"][0]["mqtt"]["format"] = "text";
        rejects([&] { catalog(mqttBad); });
        // A valid M19 store may already contain a disabled MQTT placeholder
        // without an "mqtt" object. Adding default fields changes the content
        // hash and makes current/backup/LKG appear corrupt on M20 startup.
        {
            auto oldConfig = value;
            oldConfig["connections"] = Json::array({{
                {"id","connection_legacy_mqtt"},{"name","Future broker"},
                {"type","mqtt"},{"enabled",false},{"timeout_ms",2000},
                {"network",{{"host","old_broker_host"},{"port",1883}}}
            }});
            // Underscore was allowed in disabled M19 placeholders. Changing
            // validation of dormant transports would also corrupt old stores.
            const auto historical = catalog(oldConfig);
            const auto original = Json::parse(historical->canonicalJson());
            check(!original["connections"][0].contains("mqtt"), "disabled M19 MQTT must not acquire a default mqtt object");
            const auto reloaded = config::ConfigLoader::fromJson(historical->canonicalJson());
            check(reloaded->contentHash() == historical->contentHash(), "disabled MQTT canonical hash preserved");
            Directory historicDirectory;
            config::ConfigStore historicStore(historicDirectory.path);
            historicStore.initialize(historical);
            historicStore.recordLastKnownGood(historical);
            check(historicStore.current()->contentHash() == historical->contentHash(), "historical current store readable");
            check(historicStore.revision(historical->configuration().revision)->contentHash() == historical->contentHash(),
                  "historical revision/backup readable");
            std::ofstream(historicDirectory.path + "/configuration.json") << "{broken";
            const auto recovered = historicStore.lastKnownGood();
            check(recovered && (*recovered)->contentHash() == historical->contentHash(),
                  "historical LKG readable after current corruption");
        }
        // Other planned schemas remain disabled and cannot be bound to HTTP.
        for (const auto& type : {"tcp", "udp", "serial", "can"}) {
            auto planned = value;
            Json entry{{"id","connection_planned"},{"name","Future"},{"type",type},{"enabled",false},{"timeout_ms",1000}};
            if (std::string(type) == "serial") entry["serial"] = {{"port","/dev/ttyUSB0"},{"baud_rate",115200}};
            else if (std::string(type) == "can") entry["can"] = {{"interface","can0"}};
            else entry["network"] = {{"host","192.168.1.50"},{"port",1883}};
            planned["connections"] = Json::array({entry});
            const auto snapshot = catalog(planned);
            check(snapshot->configuration().connections.size() == 1, "planned type parsed");
            check(config::ConfigLoader::fromJson(snapshot->canonicalJson())->contentHash() == snapshot->contentHash(), "planned roundtrip");
            planned["connections"][0]["enabled"] = true;
            rejects([&] { catalog(planned); });
            planned["connections"][0]["enabled"] = false;
            planned["bindings"] = Json::array({binding}); planned["bindings"][0]["connection_id"] = "connection_planned";
            rejects([&] { catalog(planned); });
        }
        std::cout << "PASS Connectivity config: canonical compatibility, HTTP/Observe roundtrip, planned schemas, AI proposal validation, stale acceptance\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
