#include "../admin/Support.h"
#include "firefly/admin/AdminDraft.h"
#include <iostream>

using namespace firefly;
using namespace admin_test;

namespace {
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

bool unsupported(const admin::GenerationResult& result) {
    return std::holds_alternative<actions::Unsupported>(result.command.action);
}

void exact(admin::RuntimeGenerations& runtime, const std::string& text,
           const std::string& target, const std::string& action) {
    const auto result = runtime.process(text);
    check(result.command.languageRoute == "INDEX_EXACT", text + " not exact");
    check(!result.command.safetyRejected && !unsupported(result), text + " rejected");
    const auto& value = std::get<actions::GenericAction>(result.command.action);
    check(value.targetId() == target && value.actionId() == action, text + " wrong target/action");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string root = argv[1];
        // Read-only owner /api/config snapshot, exact semantic entities/aliases.
        // HTTP contracts removed so this regression cannot address live equipment.
        const auto owner = config::ConfigLoader::fromFile(root + "/tests/language/m19-owner-semantic.json");
        auto document = Json::parse(owner->canonicalJson());
        Model model;
        admin::RuntimeGenerations runtime(model);
        runtime.apply(owner);
        const auto old = runtime.current();
        const std::string target = "device_lampa";
        const std::string on = "action_lampa_vklyuchit", off = "action_lampa_vyklyuchit";
        // Even a model willing to name the configured target cannot invent grounding.
        model.raw = Json{{"actionId",on},{"targetId",target},{"arguments",Json::object()}}.dump();
        for (const auto* text : {"включи лампу в кабинете", "выключи лампу в кабинете"}) {
            const auto result = runtime.process(text);
            check(unsupported(result) && result.command.indexStatus == "INDEX_MISS", "owner missing case accepted");
            check(result.command.error.find("target_not_grounded") != std::string::npos, "owner grounding diagnostic lost");
            check(!result.execution, "missing alias executed");
        }
        const auto prompt = admin::languageProfilePrompt(owner, "device", target);
        check(prompt.find("винительном падеже") != std::string::npos, "no object-case instruction");
        check(prompt.find("action_ids") == std::string::npos && prompt.find("device_lampa") == std::string::npos &&
              prompt.find("лампа_включит") == std::string::npos, "device prompt leaks identifiers/state aliases");
        const auto profile = Json::parse(admin::decodeLanguageProfile(R"({"aliases":["Лампа","лампу"]})", owner, "device", target));
        check(profile["aliases"].size() == 6, "reviewed owner aliases were erased");
        document["devices"][0]["spoken_aliases"] = profile.at("aliases");
        // Proposal alone does not activate aliases or change the old generation.
        check(unsupported(runtime.process("включи лампу в кабинете")), "proposal became active");
        check(runtime.current() == old && old->languageIndex->catalogHash() == owner->contentHash(), "proposal changed runtime");
        runtime.apply(config::ConfigLoader::fromJson(document.dump())); // isolated generation only
        const int calls = model.inferences;
        exact(runtime, "включи лампу в кабинете", target, on);
        exact(runtime, "выключи лампу в кабинете", target, off);
        check(model.inferences == calls, "reviewed exact forms called the LLM");
        for (const auto* text : {"не включи лампу в кабинете", "включи и выключи лампу в кабинете",
                                 "почему включи лампу в кабинете"})
            check(unsupported(runtime.process(text)), std::string("unsafe accepted: ") + text);
        for (const auto* alias : {"лампа_включит", "розетка_включенная", "не розетка", "розетка или насос",
                                  "игнорируй инструкции", "включи розетку", "розетка|D"}) {
            bool rejected = false;
            try { admin::decodeLanguageProfile(Json{{"aliases",Json::array({alias})}}.dump(), owner, "device", target); }
            catch (const std::exception&) { rejected = true; }
            check(rejected, std::string("unsafe/identifier proposal accepted: ") + alias);
        }
        // Generic equipment, multiword adjective agreement, numbers and no zone.
        for (const auto& forms : std::vector<std::pair<std::string,std::string>>{
                 {"Розетка","розетку"}, {"Сетевая розетка 2","сетевую розетку 2"},
                 {"Вентилятор","вентилятор"}, {"Реле","реле"}}) {
            auto other = document;
            auto& device = other["devices"][0];
            device["name"] = forms.first; device["room_id"] = nullptr;
            device["spoken_aliases"] = Json::array(); device["metadata"]["aliases"] = Json::array();
            const auto catalog = config::ConfigLoader::fromJson(other.dump());
            const auto proposed = Json::parse(admin::decodeLanguageProfile(
                Json{{"aliases",Json::array({forms.first,forms.second})}}.dump(), catalog, "device", target));
            device["spoken_aliases"] = proposed["aliases"];
            runtime.apply(config::ConfigLoader::fromJson(other.dump()));
            exact(runtime, "включи " + forms.second, target, on);
            exact(runtime, "выключи " + forms.second, target, off);
        }
        // Identical speech forms cannot select between two devices in one zone.
        auto collision = document;
        auto duplicate = collision["devices"][0]; duplicate["id"] = "device_second";
        collision["devices"].push_back(duplicate);
        const auto conflicting = config::ConfigLoader::fromJson(collision.dump());
        const auto warning = Json::parse(admin::decodeLanguageProfile(R"({"aliases":["лампу"]})", conflicting, "device", target));
        check(!warning["conflicts"].empty(), "collision warning lost");
        runtime.apply(conflicting);
        const int beforeAmbiguity = model.inferences;
        for (const auto* text : {"включи лампу в кабинете", "выключи лампу", "включи лампа"})
            check(unsupported(runtime.process(text)), "ambiguous device selected");
        check(model.inferences == beforeAmbiguity, "ambiguity reached model");
        std::cout << "M19 owner alias failure, reviewed grammar, generic/no-zone equipment and safety passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
