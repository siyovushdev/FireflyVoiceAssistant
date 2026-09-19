#include "firefly/llm/LlmContext.h"
#include "firefly/llm/StructuredOutput.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <locale>
#include <stdexcept>

namespace firefly::llm {
LlmContext generateContext(const runtime::DomainView& domain) {
    using Json = nlohmann::json;
    LlmContext result;
    result.rules = R"(Ответ только JSON: {"actionId":"ID","targetId":"ID","arguments":{"brightness":40}} либо {"unsupported":true}.
Свет/освещение/яркость: нужна ровно одна указанная комната и точное абсолютное значение. Вежливость игнорируй.
Включить/максимум=100, выключить=0, наполовину=50. Примерно/около числа означает это число.
Ярче, темнее, комфортный, немного, приглуши -> {"unsupported":true}. Это НЕ точное число. Никогда не придумывай число.
Минус/отрицательное/вне диапазона, несколько комнат/действий, другое устройство, противоречие -> unsupported.
"или"/"либо" -> {"unsupported":true}. Не выбирай одно из значений.
Дверь: только явно открыть входную дверь / дверь на входе. Закрыть/разблокировать/другая дверь -> unsupported.
По умолчанию {"unsupported":true}. Не угадывай и не исправляй.)";
    const auto& configuration = domain.snapshot()->configuration();
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "Actions: ID | name | arguments (integer min..max percent)\n";
    for (const auto& definition : configuration.actions) {
        output << definition.id << '|' << Json(definition.name).dump() << '|';
        if (definition.parameters.empty()) output << "{}";
        for (const auto& parameter : definition.parameters)
            output << parameter.id << ' ' << parameter.minimum << ".." << parameter.maximum;
        output << '\n';
    }
    const auto writeTarget = [&](const auto& target) {
        output << "  " << target.id << '|' << Json(target.name).dump() << '|';
        for (std::size_t i = 0; i < target.actionIds.size(); ++i)
            output << (i ? "," : "") << target.actionIds[i];
        output << '\n';
    };
    output << "Rooms: ID | name | aliases | locations. Indented targets: ID | name | action IDs\n";
    for (const auto& room : configuration.rooms) {
        output << room.id << '|' << Json(room.name).dump() << '|'
               << Json(room.mentions).dump() << '|' << Json(room.locations).dump() << '\n';
        for (const auto& target : configuration.devices)
            if (target.roomId && *target.roomId == room.id) writeTarget(target);
    }
    if (const auto* entrance = domain.entranceTarget()) {
        // Schema v1 permits one argument-free opening operation on this target.
        // Read the actual device/action relationship, never compact wire codes.
        for (const auto& actionId : entrance->actionIds)
            output << "Entrance " << Json(entrance->name).dump() << ": "
                   << structuredResult(actions::GenericAction(actionId, entrance->id, {})) << '\n';
    }
    result.domain = output.str();
    // Resource bound, NOT a token estimator. Actual model admission is separate.
    if (result.systemPrompt().size() > 16384) throw std::invalid_argument("Generated LLM context exceeds byte resource limit");
    return result;
}
}
