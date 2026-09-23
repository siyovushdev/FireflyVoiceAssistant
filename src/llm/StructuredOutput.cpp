#include "firefly/llm/StructuredOutput.h"
#include "firefly/actions/ScalarJson.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <map>
#include <set>

namespace firefly::llm {
namespace {
using Json = nlohmann::json;
DecodedResult reject(DecodeStatus status) { return {actions::Unsupported{}, status, {}}; }
}
const char* decodeStatusName(DecodeStatus status) {
    switch (status) {
        case DecodeStatus::NotAttempted: return "not_attempted";
        case DecodeStatus::Action: return "action";
        case DecodeStatus::Unsupported: return "unsupported";
        case DecodeStatus::InvalidJson: return "invalid_json";
        case DecodeStatus::InvalidShape: return "invalid_shape";
        case DecodeStatus::UnknownAction: return "unknown_action";
        case DecodeStatus::UnknownTarget: return "unknown_target";
        case DecodeStatus::InvalidArguments: return "invalid_arguments";
        case DecodeStatus::OutsideDomain: return "outside_domain";
    }
    return "invalid_status";
}
DecodedResult StructuredDecoder::decode(std::string_view raw) const {
    if (raw.empty() || raw.size() > MaxStructuredBytes || raw.substr(0, 3) == "\xef\xbb\xbf")
        return reject(DecodeStatus::InvalidJson);
    Json root;
    try {
        actions::checkNumericLexemes(raw);
        std::map<int, std::set<std::string>> keys;
        root = Json::parse(raw.begin(), raw.end(), [&](int depth, Json::parse_event_t event, Json& value) {
            using Event = Json::parse_event_t;
            if ((event == Event::object_start || event == Event::array_start) && depth > 2)
                throw std::invalid_argument("Structured result exceeds nesting limit");
            if (event == Event::object_start) keys[depth] = {};
            if (event == Event::object_end) keys.erase(depth);
            if (event == Event::key && !keys.at(depth - 1).insert(value.get<std::string>()).second)
                throw std::invalid_argument("Duplicate JSON member");
            return true;
        });
    } catch (const std::exception&) { return reject(DecodeStatus::InvalidJson); }
    if (!root.is_object()) return reject(DecodeStatus::InvalidShape);
    if (root.size() == 1 && root.contains("unsupported") && root["unsupported"].is_boolean() &&
        root["unsupported"].get<bool>()) return {actions::Unsupported{}, DecodeStatus::Unsupported, {}};
    if (root.size() != 3 || !root.contains("actionId") || !root["actionId"].is_string() ||
        !root.contains("targetId") || !root["targetId"].is_string() ||
        !root.contains("arguments") || !root["arguments"].is_object()) return reject(DecodeStatus::InvalidShape);
    const auto actionId = root["actionId"].get<std::string>();
    const auto targetId = root["targetId"].get<std::string>();
    if (!domain_.snapshot()->actionIndex().count(actionId)) return reject(DecodeStatus::UnknownAction);
    if (!domain_.device(targetId)) return reject(DecodeStatus::UnknownTarget);
    actions::GenericAction::Arguments arguments;
    for (const auto& [key, value] : root["arguments"].items()) {
        const auto& definition = domain_.snapshot()->configuration().actions.at(domain_.snapshot()->actionIndex().at(actionId));
        const auto parameter = std::find_if(definition.parameters.begin(),definition.parameters.end(),[&](const auto& p){return p.id==key;});
        if(parameter==definition.parameters.end())return reject(DecodeStatus::InvalidArguments);
        try { arguments.emplace(key, actions::decodeTypedScalar(value,parameter->type)); }
        catch (const std::exception&) { return reject(DecodeStatus::InvalidArguments); }
    }
    const actions::GenericAction candidate(actionId, targetId, std::move(arguments));
    if (!domain_.describe(candidate)) return {actions::Unsupported{}, DecodeStatus::InvalidArguments, domain_.validationReason(candidate)};
    const auto supported = domain_.validate(candidate);
    if (std::holds_alternative<actions::Unsupported>(supported)) return {actions::Unsupported{}, DecodeStatus::OutsideDomain, domain_.validationReason(candidate)};
    return {supported, DecodeStatus::Action, {}};
}
std::string structuredResult(const actions::CandidateResult& value) {
    const auto* action = std::get_if<actions::GenericAction>(&value);
    if (!action) return R"({"unsupported":true})";
    Json arguments = Json::object();
    for (const auto& [key, argument] : action->arguments()) arguments[key] = actions::scalarJson(argument);
    // Field order is part of our deterministic renderer, not required by JSON decoding.
    return "{\"actionId\":" + Json(action->actionId()).dump() + ",\"targetId\":" + Json(action->targetId()).dump() +
           ",\"arguments\":" + arguments.dump() + "}";
}
std::string quotedText(std::string_view value) {
    return Json(std::string(value)).dump(-1, ' ', false, Json::error_handler_t::replace);
}
}
