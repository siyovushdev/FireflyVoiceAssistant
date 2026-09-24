#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "firefly/config/ConfigStore.h"
#include "JsonInternal.h"
#include "Platform.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <type_traits>

using namespace firefly::config;
using firefly::config::detail::Json;
namespace {
struct Checks {
    int total = 0;
    void check(bool result, const std::string& message) {
        ++total;
        if (!result) throw std::runtime_error(message);
    }
    template<class F> void rejects(const std::string& code, F operation) {
        ++total;
        try { operation(); }
        catch (const ConfigError& error) {
            if (code.empty() || error.code == code) return;
            throw std::runtime_error("Expected " + code + ", received " + error.code + " at " + error.path + ": " + error.what());
        }
        throw std::runtime_error("Expected rejection " + code);
    }
};
struct Scratch {
    std::string path;
    Scratch() {
        char pattern[] = "/tmp/firefly-config-test-XXXXXX";
        const char* directory = ::mkdtemp(pattern);
        if (!directory) throw std::runtime_error("mkdtemp failed");
        path = directory;
    }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void write(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary); output << text;
    if (!output) throw std::runtime_error("fixture write failed");
}
ExpectedRevision expected(const Catalog& catalog) { return {catalog->configuration().revision, catalog->contentHash()}; }
void validation(Checks& test, const std::string& root) {
    const auto text = detail::readFile(root + "/config/current-m4.json");
    const auto source = Json::parse(text);
    test.check(ConfigLoader::fromJson(text)->matchesCurrentLegacyDomain(), "current domain validates");
    const auto bad = [&](const std::string& code, const auto& mutate) {
        auto value = source; mutate(value);
        test.rejects(code, [&] { (void)ConfigLoader::fromJson(value.dump()); });
    };
    for (const auto& textValue : {"", "{", "null", "[]", "{}", "{\"schema_version\":1} trailing", "/*x*/{}", "{\"x\":NaN}"})
        test.rejects("", [&] { (void)ConfigLoader::fromJson(textValue); });
    for (const auto& path : {"/schema_version", "/revision", "/retired_ids", "/system", "/rooms", "/devices", "/actions", "/safety", "/bindings", "/compatibility"}) {
        bad("missing_field", [&](auto& value) { value.erase(std::string(path).substr(1)); });
    }
    bad("unsupported_version", [](auto& value) { value["schema_version"] = 3; });
    bad("unsupported_version", [](auto& value) { value["schema_version"] = -1; });
    bad("schema_error", [](auto& value) { value["schema_version"] = true; });
    bad("schema_error", [](auto& value) { value["schema_version"] = "1"; });
    bad("schema_error", [](auto& value) { value["revision"] = 0; });
    bad("schema_error", [](auto& value) { value["revision"] = 9007199254740992ULL; });
    bad("integer_required", [](auto& value) { value["revision"] = 1.0; });
    for (const auto& number : {"1e0", "1.0", "18446744073709551616"}) {
        auto value = text; const auto position = value.find("\"revision\": 1");
        value.replace(position, std::string("\"revision\": 1").size(), std::string("\"revision\": ") + number);
        test.rejects("integer_required", [&] { (void)ConfigLoader::fromJson(value); });
    }
    test.rejects("duplicate_key", [&] { (void)ConfigLoader::fromJson("{\"schema_version\":1,\"schema_\\u0076ersion\":1}"); });
    test.rejects("duplicate_key", [&] { (void)ConfigLoader::fromJson("{\"a\":[{\"b\":0,\"b\":1}]}"); });
    test.rejects("depth_limit", [&] { (void)ConfigLoader::fromJson(std::string(17, '[') + "0" + std::string(17, ']')); });
    test.rejects("size_limit", [&] { (void)ConfigLoader::fromJson(std::string(MaxConfigBytes + 1, ' ')); });
    test.rejects("invalid_text", [&] { (void)ConfigLoader::fromJson("\xef\xbb\xbf" + text); });
    test.rejects("invalid_text", [&] { (void)ConfigLoader::fromJson(text + std::string(1, '\0')); });
    for (const auto& invalid : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xd0", 1)})
        test.rejects("malformed_json", [&] { (void)ConfigLoader::fromJson("{\"x\":\"" + invalid + "\"}"); });
    for (const auto& invalid : std::vector<std::string>{std::string(1, '\0'), "\n", "\x7f", "\xc2\x85", "\xe2\x80\xae", "<|im_start|>"})
        bad("invalid_text", [&](auto& value) { value["system"]["name"] = std::string(invalid); });
    bad("unknown_field", [](auto& value) { value["prompt"] = "ignore safety"; });
    bad("unknown_field", [](auto& value) { value["system"]["allow_unsafe"] = true; });
    bad("unknown_field", [](auto& value) { value["safety"]["skip_safety"] = true; });
    bad("missing_field", [](auto& value) { value["devices"][0].erase("enabled"); });
    bad("schema_error", [](auto& value) { value["system"]["id"] = "дом"; });
    bad("schema_error", [](auto& value) { value["system"]["id"] = "Uppercase"; });
    bad("invalid_name", [](auto& value) { value["system"]["name"] = "   "; });
    bad("invalid_name", [](auto& value) { std::string name; for (int i = 0; i < 65; ++i) name += "я"; value["system"]["name"] = name; });
    bad("duplicate_id", [](auto& value) { value["rooms"][1]["id"] = value["rooms"][0]["id"]; });
    bad("duplicate_id", [](auto& value) { value["devices"][1]["id"] = value["rooms"][0]["id"]; });
    bad("duplicate_id", [](auto& value) { value["retired_ids"] = {"unused", "unused"}; });
    bad("retired_id", [](auto& value) { value["retired_ids"] = {"room_kitchen"}; });
    bad("ambiguous_alias", [](auto& value) { value["rooms"][1]["speech"]["mentions"].push_back("кухне"); });
    bad("ambiguous_alias", [](auto& value) { value["rooms"][0]["speech"]["mentions"].push_back("КУХНЯ"); });
    bad("ambiguous_alias", [](auto& value) { value["rooms"][1]["speech"]["locations"] = {"на кухне"}; });
    bad("ambiguous_alias", [](auto& value) { value["rooms"][1]["speech"]["locations"] = {"около кухни"}; });
    bad("invalid_alias", [](auto& value) { value["rooms"][0]["speech"]["locations"] = {"на  кухне"}; });
    bad("invalid_alias", [](auto& value) { value["rooms"][0]["speech"]["mentions"] = {"моя кухня"}; });
    // Natural grammatical pairs belong to one location without literal substring equality.
    {
        auto officeBase = ConfigValidator::validate(migrateV2(ConfigLoader::fromJson(source.dump())->configuration()));
        auto value = Json::parse(officeBase->canonicalJson());
        value["rooms"].push_back({{"id","location_office_1"},{"name","Кабинет 1"},
            {"speech",{{"mentions",{"кабинет 1"}},{"locations",{"в кабинете 1"}}}},
            {"metadata",{{"type","office"},{"description","Офисный кабинет."},{"aliases",Json::array()},{"parent_id",nullptr},{"container",false}}}});
        const auto accepted = ConfigLoader::fromJson(value.dump());
        test.check(accepted->roomIndex().count("location_office_1"), "кабинет 1 + в кабинете 1 validates");
        const auto& kitchen = accepted->configuration().rooms[accepted->roomIndex().at("room_kitchen")];
        test.check(std::find(kitchen.locations.begin(), kitchen.locations.end(), "на кухне") != kitchen.locations.end() &&
            std::find(kitchen.mentions.begin(), kitchen.mentions.end(), "кухня") != kitchen.mentions.end(),
            "кухня / на кухне remains valid");
        auto collision = Json::parse(accepted->canonicalJson());
        collision["rooms"].push_back({{"id","location_office_2"},{"name","Кабинет 2"},
            {"speech",{{"mentions",{"кабинет 2"}},{"locations",{"в кабинете 1"}}}},
            {"metadata",{{"type","office"},{"description","Другой кабинет."},{"aliases",Json::array()},{"parent_id",nullptr},{"container",false}}}});
        test.rejects("ambiguous_alias", [&] { (void)ConfigLoader::fromJson(collision.dump()); });
    }
    bad("reserved_alias", [](auto& value) { value["rooms"][0]["speech"]["mentions"].push_back("открой"); });
    bad("reserved_alias", [](auto& value) { value["rooms"][0]["speech"]["mentions"].push_back("не"); });
    bad("dangling_reference", [](auto& value) { value["devices"][0]["room_id"] = "unknown"; });
    bad("dangling_reference", [](auto& value) { value["devices"][0]["action_ids"] = {"unknown"}; });
    bad("duplicate_reference", [](auto& value) { value["devices"][0]["action_ids"].push_back("action_set_brightness"); });
    bad("unsafe_constraint", [](auto& value) { value["devices"][0]["room_id"] = nullptr; });
    bad("unsafe_constraint", [](auto& value) { value["devices"][4]["room_id"] = "room_corridor"; });
    bad("unsafe_constraint", [](auto& value) { value["devices"][0]["action_ids"] = {"action_open_entrance"}; });
    bad("schema_error", [](auto& value) { value["devices"][0]["kind"] = "thermostat"; value["devices"][0]["enabled"] = false; });
    bad("schema_error", [](auto& value) { value["actions"][0]["operation"] = "execute.shell"; });
    bad("schema_error", [](auto& value) { value["actions"][0]["parameters"][0]["minimum"] = -1; });
    bad("schema_error", [](auto& value) { value["actions"][0]["parameters"][0]["maximum"] = 101; });
    bad("unsafe_constraint", [](auto& value) { value["actions"][0]["parameters"][0]["minimum"] = 80; value["actions"][0]["parameters"][0]["maximum"] = 20; });
    bad("schema_error", [](auto& value) { value["actions"][0]["parameters"][0]["required"] = false; });
    bad("schema_error", [](auto& value) { value["actions"][0]["parameters"][0]["type"] = "string"; });
    bad("schema_error", [](auto& value) { value["actions"][0]["parameters"][0]["unit"] = "raw"; });
    bad("unsupported_operation", [](auto& value) { value["actions"][0]["parameters"] = Json::array(); });
    bad("duplicate_id", [](auto& value) { value["actions"][0]["parameters"].push_back(value["actions"][0]["parameters"][0]); });
    bad("unsafe_constraint", [](auto& value) { value["actions"][1]["parameters"] = value["actions"][0]["parameters"]; });
    bad("schema_error", [](auto& value) { value["bindings"] = {{{"driver", "gpio"}}}; });
    // Schema v2 admits typed HTTP bindings; unknown executors and bad URLs fail closed.
    {
        auto migrated = Json::parse(ConfigValidator::validate(migrateV2(ConfigLoader::fromJson(source.dump())->configuration()))->canonicalJson());
        migrated["bindings"] = Json::array({{{"target_id","device_kitchen_light"},{"action_id","action_set_brightness"},
            {"executor","http"},{"http",{{"method","POST"},{"url","http://127.0.0.1:9/brightness"},{"timeout_ms",500},
            {"content_type","application/json"},{"body",Json::object()},{"argument","brightness"}}}}});
        const auto accepted = ConfigLoader::fromJson(migrated.dump());
        test.check(accepted->bindingIndex().count({"device_kitchen_light","action_set_brightness"}), "v2 HTTP binding validates");
        auto duplicate = migrated; duplicate["bindings"].push_back(migrated["bindings"][0]);
        test.rejects("duplicate_binding", [&]{ (void)ConfigLoader::fromJson(duplicate.dump()); });
        auto badUrl = migrated; badUrl["bindings"][0]["http"]["url"] = "https://example.com/x";
        test.rejects("malformed_binding", [&]{ (void)ConfigLoader::fromJson(badUrl.dump()); });
        auto hostname = migrated; hostname["bindings"][0]["http"]["url"] = "http://esp32.local/relay";
        test.rejects("malformed_binding", [&]{ (void)ConfigLoader::fromJson(hostname.dump()); });
        auto unknownTarget = migrated; unknownTarget["bindings"][0]["target_id"] = "device_missing";
        test.rejects("dangling_reference", [&]{ (void)ConfigLoader::fromJson(unknownTarget.dump()); });
    }
    bad("schema_error", [](auto& value) { value["devices"][0]["spoken_aliases"] = {"лампа"}; });
    bad("unknown_field", [](auto& value) { value["actions"][1]["speech_aliases"]["open_verb"] = {"запусти"}; });
    bad("legacy_incompatible", [](auto& value) { std::swap(value["compatibility"]["light_targets"]["K"], value["compatibility"]["light_targets"]["B"]); });
    bad("dangling_reference", [](auto& value) { value["compatibility"]["door_target_id"] = "unknown"; });
    bad("unknown_field", [](auto& value) { value["compatibility"]["light_targets"]["X"] = "device_kitchen_light"; });
    const Json deny{{"id", "deny_door"}, {"name", "Запрет открытия"}, {"kind", "deny_action"}, {"device_id", "device_entrance_door"}, {"action_id", "action_open_entrance"}};
    auto restricted = source; restricted["safety"]["restrictions"].push_back(deny);
    const auto restrictedCatalog = ConfigLoader::fromJson(restricted.dump());
    test.check(!restrictedCatalog->matchesCurrentLegacyDomain(), "deny differs from baseline");
    const auto door = std::find_if(restrictedCatalog->effectiveActions().begin(), restrictedCatalog->effectiveActions().end(), [](const auto& a) { return a.operation == Operation::OpenEntranceDoor; });
    test.check(door != restrictedCatalog->effectiveActions().end() && !door->permitted && !door->mandatoryPolicies.empty(), "deny cannot remove mandatory door policy");
    restricted["safety"]["restrictions"][0]["device_id"] = "unknown";
    test.rejects("dangling_reference", [&] { (void)ConfigLoader::fromJson(restricted.dump()); });
    restricted["safety"]["restrictions"][0] = deny;
    restricted["safety"]["restrictions"][0]["kind"] = "allow_action";
    test.rejects("schema_error", [&] { (void)ConfigLoader::fromJson(restricted.dump()); });
    auto narrowed = source; narrowed["actions"][0]["parameters"][0]["maximum"] = 80;
    test.check(!ConfigLoader::fromJson(narrowed.dump())->matchesCurrentLegacyDomain(), "tighter bounds stored but not baseline compatible");
    Scratch scratch;
    write(scratch.path + "/huge.json", std::string(MaxConfigBytes + 1, ' '));
    test.rejects("size_limit", [&] { (void)ConfigLoader::fromFile(scratch.path + "/huge.json"); });
    ::symlink((root + "/config/current-m4.json").c_str(), (scratch.path + "/link").c_str());
    test.rejects("io_error", [&] { (void)ConfigLoader::fromFile(scratch.path + "/link"); });
    ::symlink((root + "/config").c_str(), (scratch.path + "/directory-link").c_str());
    test.rejects("io_error", [&] { (void)ConfigLoader::fromFile(scratch.path + "/directory-link/current-m4.json"); });
    ::mkfifo((scratch.path + "/fifo").c_str(), 0600);
    test.rejects("invalid_file", [&] { (void)ConfigLoader::fromFile(scratch.path + "/fifo"); });
    test.rejects("invalid_file", [&] { (void)ConfigLoader::fromFile(scratch.path); });
}
void snapshot(Checks& test, const std::string& root) {
    const auto base = ConfigLoader::fromFile(root + "/config/current-m4.json");
    static_assert(std::is_const_v<std::remove_reference_t<decltype(base->configuration())>>);
    static_assert(std::is_const_v<typename Catalog::element_type>);
    auto mutableConfiguration = base->configuration();
    const auto copy = ConfigValidator::validate(mutableConfiguration);
    mutableConfiguration.rooms[0].name = "Changed outside snapshot";
    mutableConfiguration.devices.clear();
    test.check(copy->canonicalJson() == base->canonicalJson(), "snapshot owns a deep copy");
    test.check(base->summary() == detail::readFile(root + "/tests/config/current-summary.json"), "independent golden summary");
    auto shuffled = Json::parse(base->canonicalJson());
    for (const auto* key : {"rooms", "devices", "actions"}) std::reverse(shuffled[key].begin(), shuffled[key].end());
    for (auto& room : shuffled["rooms"]) for (auto& aliases : room["speech"].items()) std::reverse(aliases.value().begin(), aliases.value().end());
    const auto reordered = ConfigLoader::fromJson(shuffled.dump());
    test.check(reordered->summary() == base->summary(), "order-independent normalized output");
    test.check(ConfigLoader::fromJson(base->canonicalJson())->canonicalJson() == base->canonicalJson(), "canonical round trip");
    test.check(base->roomIndex().size() == 4 && base->deviceIndex().size() == 5 && base->actionIndex().size() == 2, "complete ID indexes");
    test.check(base->mentionIndex().at("зал") == "room_living" && base->locationIndex().at("в зале") == "room_living", "distinct alias roles share stable target");
    auto renamed = base->configuration(); renamed.rooms[0].name = "Новое имя";
    const auto rename = ConfigValidator::validate(renamed);
    test.check(rename->roomIndex() == base->roomIndex() && rename->mentionIndex() == base->mentionIndex(), "display rename preserves identities and speech");
    test.check(rename->matchesCurrentLegacyDomain(), "cosmetic rename does not alter legacy interpretation domain");
    renamed.schemaVersion = 3;
    test.rejects("unsupported_version", [&] { (void)ConfigValidator::validate(renamed); });
    renamed = base->configuration(); renamed.devices[0].kind = static_cast<DeviceKind>(99);
    test.rejects("unsupported_kind", [&] { (void)ConfigValidator::validate(renamed); });
    renamed = base->configuration(); renamed.actions[0].operation = static_cast<Operation>(99);
    test.rejects("unsupported_operation", [&] { (void)ConfigValidator::validate(renamed); });
    test.check(detail::sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty vector");
    test.check(detail::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known vector");
}
void storeTests(Checks& test, const std::string& root) {
    const auto base = ConfigLoader::fromFile(root + "/config/current-m4.json");
    auto renamedValue = base->configuration(); renamedValue.system.name = "Изменённый дом";
    const auto renamed = ConfigValidator::validate(renamedValue);
    Scratch scratch; ConfigStore store(scratch.path);
    const auto first = store.initialize(base);
    test.check(first->configuration().revision == 1 && store.current()->contentHash() == first->contentHash(), "initialize canonical store");
    test.rejects("already_initialized", [&] { (void)store.initialize(base); });
    test.rejects("revision_conflict", [&] { (void)store.save(renamed, {2, first->contentHash()}); });
    test.rejects("revision_conflict", [&] { (void)store.save(renamed, {1, std::string(64, '0')}); });
    const auto second = store.save(renamed, expected(first));
    test.check(second->configuration().revision == 2, "save increments revision");
    test.check(store.revisions().size() == 1 && store.revisions()[0].hash == first->contentHash(), "backup preserves exact preceding identity");
    const auto third = store.restore(1, expected(second));
    test.check(third->configuration().revision == 3 && third->configuration().system.name == first->configuration().system.name, "explicit restore is a new revision");
    test.rejects("unknown_revision", [&] { (void)store.restore(99, expected(third)); });
    auto added = third->configuration();
    Room extra; extra.id = "room_office"; extra.name = "Кабинет"; extra.mentions = {"кабинет", "кабинете"}; extra.locations = {"в кабинете"};
    added.rooms.push_back(extra);
    const auto fourth = store.save(ConfigValidator::validate(added), expected(third));
    test.check(!fourth->matchesCurrentLegacyDomain(), "extra unbound data never claims legacy compatibility");
    auto reassigned = third->configuration();
    DenyAction replacement; replacement.id = extra.id; replacement.name = "Запрет";
    replacement.deviceId = "device_entrance_door"; replacement.actionId = "action_open_entrance";
    reassigned.restrictions.push_back(replacement);
    test.rejects("identity_conflict", [&] { (void)store.save(ConfigValidator::validate(reassigned), expected(fourth)); });
    auto deleted = fourth->configuration();
    deleted.rooms.erase(std::remove_if(deleted.rooms.begin(), deleted.rooms.end(), [](const auto& room) { return room.id == "room_office"; }), deleted.rooms.end());
    const auto fifth = store.save(ConfigValidator::validate(deleted), expected(fourth));
    test.check(fifth->configuration().retiredIds == std::vector<std::string>{"room_office"}, "deletion retires ID in same transaction");
    test.rejects("retired_id", [&] { (void)store.save(fourth, expected(fifth)); });
    test.rejects("retired_id", [&] { (void)store.restore(4, expected(fifth)); });
    auto eraseHistory = fifth->configuration(); eraseHistory.retiredIds.clear();
    const auto sixth = store.save(ConfigValidator::validate(eraseHistory), expected(fifth));
    test.check(sixth->configuration().retiredIds == fifth->configuration().retiredIds, "ordinary save cannot erase retired IDs");
    test.check(store.revisions().size() == 5, "successful history retained");
    auto latest = sixth;
    for (int i = 0; i < 7; ++i) latest = store.save(latest, expected(latest));
    test.check(store.revisions().size() == 12 && store.revisions().front().revision == 1, "history is retained beyond ten revisions without unsafe pruning");
    for (const auto step : {StoreStep::BackupWrite, StoreStep::BackupSync, StoreStep::BackupPublish, StoreStep::BackupDirectorySync,
                           StoreStep::Write, StoreStep::FileSync, StoreStep::Rename, StoreStep::DirectorySync}) {
        Scratch temp; const auto initial = ConfigStore(temp.path).initialize(base);
        bool injected = false;
        ConfigStore failing(temp.path, [&](StoreStep at) { if (at == step) { injected = true; throw ConfigError("injected_io", "", "Simulated EIO/ENOSPC"); } });
        test.rejects(step == StoreStep::DirectorySync ? "durability_indeterminate" : "injected_io", [&] { (void)failing.save(renamed, expected(initial)); });
        test.check(injected, "fault seam was exercised");
        if (step == StoreStep::DirectorySync) {
            test.rejects("recovery_required", [&] { (void)ConfigStore(temp.path).save(renamed, expected(initial)); });
            test.check(ConfigStore(temp.path).reconcile()->configuration().revision == 2, "explicit reconciliation accepts only visible new state");
        } else test.check(ConfigStore(temp.path).current()->canonicalJson() == initial->canonicalJson(), "precommit failures preserve exact old bytes");
    }
    for (const auto stage : {StoreStep::Write, StoreStep::DirectorySync}) {
        Scratch temp; const auto initial = ConfigStore(temp.path).initialize(base);
        const auto child = ::fork();
        if (child < 0) throw std::runtime_error("fork failed");
        if (child == 0) {
            ConfigStore crash(temp.path, [&](StoreStep at) { if (at == stage) ::_exit(9); });
            (void)crash.save(renamed, expected(initial)); ::_exit(10);
        }
        int status = 0; ::waitpid(child, &status, 0);
        test.check(WIFEXITED(status) && WEXITSTATUS(status) == 9, "process interrupted at publication boundary");
        test.rejects("recovery_required", [&] { (void)ConfigStore(temp.path).current(); });
        const auto recovered = ConfigStore(temp.path).reconcile();
        test.check(recovered->configuration().revision == (stage == StoreStep::Write ? 1 : 2), "no orphan temp promotion after crash");
    }
    {
        Scratch temp; const auto child = ::fork();
        if (child == 0) { ConfigStore crash(temp.path, [](StoreStep at) { if (at == StoreStep::Write) ::_exit(9); }); (void)crash.initialize(base); ::_exit(10); }
        if (child < 0) throw std::runtime_error("fork failed");
        int status = 0; ::waitpid(child, &status, 0);
        test.check(!ConfigStore(temp.path).reconcile(), "interrupted first initialization has no committed catalog");
        test.check(ConfigStore(temp.path).initialize(base)->configuration().revision == 1, "explicit reinitialization after abort");
    }
    {
        Scratch temp; const auto initial = ConfigStore(temp.path).initialize(base);
        pid_t children[2];
        for (auto& child : children) {
            child = ::fork();
            if (child < 0) throw std::runtime_error("fork failed");
            if (child == 0) {
                try { (void)ConfigStore(temp.path).save(renamed, expected(initial)); ::_exit(0); }
                catch (const ConfigError& e) { ::_exit(e.code == "revision_conflict" ? 10 : 20); }
            }
        }
        int winners = 0, conflicts = 0;
        for (const auto child : children) { int status = 0; ::waitpid(child, &status, 0); if (WIFEXITED(status)) { winners += WEXITSTATUS(status) == 0; conflicts += WEXITSTATUS(status) == 10; } }
        test.check(winners == 1 && conflicts == 1 && ConfigStore(temp.path).current()->configuration().revision == 2, "concurrent writers serialize and reject stale update");
    }
    {
        Scratch temp; ::symlink((root + "/config/current-m4.json").c_str(), (temp.path + "/configuration.json").c_str());
        test.rejects("io_error", [&] { (void)ConfigStore(temp.path).current(); });
        test.rejects("already_initialized", [&] { (void)ConfigStore(temp.path).initialize(base); });
    }
    {
        Scratch temp; ::chmod(temp.path.c_str(), 0777);
        test.rejects("unsafe_store", [&] { (void)ConfigStore(temp.path).initialize(base); });
    }
    {
        Scratch temp; (void)ConfigStore(temp.path).initialize(base);
        (void)ConfigStore(temp.path).save(renamed, expected(base));
        write(temp.path + "/configuration.json", "{}");
        test.rejects("missing_field", [&] { (void)ConfigStore(temp.path).current(); });
    }
    {
        Scratch temp; (void)ConfigStore(temp.path).initialize(base);
        write(temp.path + "/.pending.json", "{}");
        test.rejects("recovery_required", [&] { (void)ConfigStore(temp.path).current(); });
        test.rejects("store_corrupt", [&] { (void)ConfigStore(temp.path).reconcile(); });
    }
}
}
int main(int argc, char** argv) {
    Checks test;
    try {
        if (argc != 3) throw std::runtime_error("Expected suite and repository root");
        const std::string suite(argv[1]);
        if (suite == "Validation") validation(test, argv[2]);
        else if (suite == "Snapshot") snapshot(test, argv[2]);
        else if (suite == "Store") storeTests(test, argv[2]);
        else throw std::runtime_error("Unknown suite");
        std::cout << suite << ": " << test.total << " checks passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << test.total << " checks: " << error.what() << '\n'; return 1;
    }
}
