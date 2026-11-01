#include "firefly/dialogue/OperationalDialogue.h"
#include "firefly/runtime/DomainView.h"
#include "../Text.h"
#include <algorithm>
#include <set>
#include <sstream>

namespace firefly::dialogue {
namespace {
using execution::StateKnowledge;
std::string bounded(std::string text, std::size_t limit = 120) {
    if (text.size() <= limit) return text;
    while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80) --limit;
    return text.substr(0, limit) + "…";
}
bool has(const std::vector<std::string>& words, std::initializer_list<const char*> values) {
    for (const auto* v : values) if (std::find(words.begin(), words.end(), v) != words.end()) return true;
    return false;
}
const config::Device* device(const Facts& f, const std::string& id) {
    const auto& all = f.catalog->configuration().devices;
    const auto it = std::find_if(all.begin(), all.end(), [&](const auto& d) { return d.id == id; });
    return it == all.end() ? nullptr : &*it;
}
const execution::DeviceKnowledge* state(const Facts& f, const std::string& id) {
    const auto it = std::find_if(f.devices.begin(), f.devices.end(), [&](const auto& d) { return d.targetId == id; });
    return it == f.devices.end() ? nullptr : &*it;
}
std::string scalar(const actions::Scalar& v) {
    if (const auto* b = std::get_if<bool>(&v.data())) return *b ? "да" : "нет";
    if (const auto* i = std::get_if<std::int64_t>(&v.data())) return std::to_string(*i);
    if (const auto* n = std::get_if<double>(&v.data())) { std::ostringstream s; s << *n; return s.str(); }
    return bounded(std::get<std::string>(v.data()));
}
std::string stateText(const Facts& f, const config::Device& d, bool detailed) {
    std::string result = "«" + bounded(d.name) + "»: ";
    const auto* known = state(f, d.id);
    if (!known || known->properties.empty()) return result + "состояние неизвестно.";
    int count = 0;
    for (const auto& [id, p] : known->properties) {
        if (++count > (detailed ? 5 : 2)) { result += "Другие свойства — в Web Admin."; break; }
        std::string name = id;
        for (const auto& field : d.state) if (field.id == id) name = field.name;
        if (id == "brightness" && d.kind == config::DeviceKind::Light) name = "Яркость";
        if (id == "position" && d.kind == config::DeviceKind::EntranceDoor) name = "Положение";
        result += bounded(name) + ": ";
        if (!p.value || p.knowledge == StateKnowledge::Unknown) { result += "неизвестно. "; continue; }
        if (execution::isStale(p, f.steadyMs)) { result += "показание устарело, текущее значение неизвестно. "; continue; }
        const bool power = id == "power" || id == "on" || id == "running";
        std::string value = scalar(*p.value);
        if (power && std::holds_alternative<bool>(p.value->data())) value = std::get<bool>(p.value->data()) ? "включено" : "выключено";
        if (p.knowledge == StateKnowledge::Predicted) result += "ожидается «" + value + "», физически не подтверждено. ";
        else result += "по подтверждённому наблюдению «" + value + "». ";
    }
    if (known->connectivity == execution::Connectivity::Unreachable) result += "Последняя попытка связи завершилась ошибкой; это не доказывает выключение устройства.";
    else if (detailed) {
        if (known->connectivity == execution::Connectivity::Reachable) result += "Канал связи отвечал; доступность канала не равна физическому состоянию.";
        else result += "Доступность канала неизвестна.";
    }
    return result;
}
std::string failure(const std::string& reason) {
    if (reason == "binding_missing" || reason == "binding_ambiguous") return "Не настроено однозначное подключение действия. Команда не отправлена.";
    if (reason == "connection_disabled" || reason == "unsupported_executor" || reason == "mqtt_unavailable" || reason == "mqtt_connection_missing")
        return "Подключение недоступно. Команда не отправлена.";
    if (reason == "unsupported" || reason == "grounding" || reason == "safety" || reason == "domain" || reason == "model_unsupported" || reason == "command_guard")
        return "Не удалось безопасно и однозначно определить разрешённую команду. Ничего не отправлено.";
    if (reason == "decoder" || reason == "runtime") return "Не удалось обработать команду безопасно. Она не отправлена устройству.";
    if (reason == "recognition") return "Не удалось распознать просьбу. Ничего не отправлено.";
    return "Не удалось подтвердить выполнение команды: устройство или исполнитель не ответили корректно. Результат неизвестен, автоматического повтора нет.";
}
std::string executionText(const Facts& f, const execution::ExecutionResult& e, bool detailed) {
    if (!e.success || e.status != execution::ExecutionStatus::Accepted) return failure(e.error);
    const auto* d = device(f, e.targetId);
    if (!d) return "В этой сессии команда была принята для устройства из прежней конфигурации. Его текущее состояние неизвестно.";
    std::string name = "действие";
    for (const auto& a : f.catalog->configuration().actions) if (a.id == e.actionId) name = a.name;
    auto text = "Команда «" + bounded(name) + "» для «" + bounded(d->name) + "» принята исполнителем. ";
    // Always distinguish transport acceptance from observation, even if a
    // separate current observation is already available by response time.
    text += stateText(f, *d, detailed);
    return text;
}
bool production(const Event& e) { return e.source == "voice" || e.source == "admin-production" || e.source == "admin-device"; }
std::optional<std::pair<StateKnowledge, bool>> powered(const Facts& f, const config::Device& d) {
    const auto* s = state(f, d.id); if (!s) return {};
    for (const auto& key : {"power", "on", "running", "brightness"}) {
        const auto p = s->properties.find(key);
        if (p == s->properties.end() || !p->second.value || p->second.knowledge == StateKnowledge::Unknown || execution::isStale(p->second, f.steadyMs)) continue;
        const auto& v = p->second.value->data();
        if (std::holds_alternative<bool>(v) && std::string(key) != "brightness") return std::make_pair(p->second.knowledge, std::get<bool>(v));
        if (std::string(key) == "brightness" && d.kind == config::DeviceKind::Light && std::holds_alternative<std::int64_t>(v)) return std::make_pair(p->second.knowledge, std::get<std::int64_t>(v) > 0);
    }
    return {};
}
}
bool OperationalDialogue::questionLike(std::string_view original) {
    const auto w = detail::words(detail::lowercase(original));
    if (has(w, {"почему", "зачем", "что", "где", "как", "когда", "какие", "какое", "какая", "какой", "сколько", "ли", "состояние", "статус"})) return true;
    if (has(w, {"пусть", "пускай"})) return false;
    if (has(w, {"ты", "ассистент"}) && has(w, {"работаешь", "готова", "доступна", "умеешь"})) return true;
    if (has(w, {"работает", "работают", "горит", "горят", "включено", "выключено", "включен", "выключен", "включён", "выключён"})) return true;
    if (has(w, {"ты", "вы"}) && has(w, {"сделала", "сделал", "включила", "выключила", "отключила", "смогла", "выполнила", "приняла"})) return true;
    const std::set<std::string> continuation{"на", "в", "там", "он", "она", "оно", "это"};
    return w.size() > 1 && w.front() == "а" && continuation.count(w[1]);
}
void OperationalDialogue::clear() { std::lock_guard<std::mutex> lock(mutex_); context_ = {}; attempt_.reset(); }
void OperationalDialogue::rememberAttempt(const Facts& f, const std::optional<execution::ExecutionResult>& e, const std::string& rejection) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t seen = 0;
    for (const auto& event : f.history) if (production(event)) seen = std::max(seen, event.sequence);
    attempt_ = Attempt{f.catalog->contentHash(), rejection, e, seen};
    context_ = {};
    if (e && device(f, e->targetId)) context_ = {f.catalog->contentHash(), e->targetId, {}, f.steadyMs, 0, Question::State};
}
std::string OperationalDialogue::commandReply(const Facts& f, const std::optional<execution::ExecutionResult>& e, const std::string& rejection, bool detailed) {
    return bounded(e ? executionText(f, *e, detailed) : failure(rejection), 1000);
}
Reply OperationalDialogue::ask(std::string_view original, const Facts& f, bool enabled, bool detailed, const Classifier& classify) {
    if (!questionLike(original)) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    Reply reply{true, {}, {}, Question::Unclear};
    if (!enabled) { reply.text = "Вопросы о работе ассистента выключены в настройках."; reply.reason = "questions_disabled"; return reply; }
    if (original.size() > 1024) { reply.text = "Сформулируйте один короткий вопрос о работе ассистента."; reply.reason = "input_limit"; return reply; }
    if (context_.hash != f.catalog->contentHash() || f.steadyMs < context_.at || f.steadyMs - context_.at > 120000 || context_.turns >= 6) context_ = {};
    const auto w = detail::words(detail::lowercase(original));
    std::string referent;
    Question kind = Question::Unclear;
    if (has(w, {"почему", "ошибка", "ошибки", "ошибку", "отказ", "отказа"})) kind = Question::Failure;
    else if (has(w, {"последним", "последнее", "последнюю", "сделала", "сделал", "включила", "выключила", "отключила", "выполнила", "приняла"})) kind = Question::LastAction;
    else if ((has(w, {"умеешь", "можешь", "возможности"}) && has(w, {"что", "как"})) || (has(w, {"ты", "ассистент"}) && has(w, {"работаешь", "готова", "доступна"}))) kind = Question::Capabilities;
    else if (has(w, {"что", "какие"}) && has(w, {"включено", "включены", "включен", "включён", "работает", "работают", "горит", "горят"})) kind = Question::OnList;
    else if (has(w, {"состояние", "статус", "работает", "работают", "горит", "горят", "включено", "включен", "включён", "выключен", "выключён", "выключено", "яркость", "температура", "положение"})) kind = Question::State;
    const bool follow = w.size() > 1 && w.front() == "а";
    if (kind == Question::Unclear && follow) {
        if (context_.question == Question::State || context_.question == Question::OnList || context_.question == Question::LastAction) kind = context_.question;
        else { reply.text = "Уточните, что нужно узнать о названной зоне или устройстве."; reply.reason = "context_missing"; return reply; }
    }
    if (kind == Question::Unclear && classify) kind = classify(original);
    if (kind == Question::None || kind == Question::Unclear) {
        reply.text = "Могу ответить о состоянии настроенных устройств, последней команде и причине отказа. Уточните вопрос."; reply.reason = "outside_operational_scope"; return reply;
    }
    reply.question = kind;
    runtime::DomainView domain(f.catalog);
    const auto grounding = domain.resolve(original, false);
    std::vector<std::string> targets = grounding.devices;
    auto locations = grounding.locations;
    if (grounding.ambiguous || locations.size() > 1) {
        reply.text = "Уточните устройство и зону: вопрос неоднозначен."; reply.reason = "ambiguous"; return reply;
    }
    if (targets.empty() && locations.size() == 1) {
        for (const auto& d : f.catalog->configuration().devices) if (d.roomId == locations.front()) targets.push_back(d.id);
    }
    if (targets.empty() && locations.empty() && has(w, {"он", "она", "оно", "его", "ее", "её", "это"})) {
        const std::set<std::string> allowed{"а", "он", "она", "оно", "его", "ее", "её", "это", "сейчас", "работает", "горит", "включен", "включён", "выключен", "выключён", "ли", "уже", "еще", "ещё", "точно", "теперь", "не", "почему", "включается", "выключается", "включился", "выключился", "ответил", "ответила", "заработал"};
        const bool clean = std::all_of(w.begin(), w.end(), [&](const auto& word) { return allowed.count(word); });
        if (clean && !context_.target.empty() && device(f, context_.target)) targets.push_back(context_.target);
        else { reply.text = "О каком устройстве вы спрашиваете? Назовите его и зону."; reply.reason = "context_missing"; return reply; }
    }
    if (locations.size() == 1 && targets.empty()) {
        reply.text = "В названной зоне нет настроенных устройств."; reply.reason = "empty_location"; return reply;
    }
    std::set<std::string> coveredWords;
    for (const auto& mention : grounding.mentions) for (const auto& word : detail::words(detail::lowercase(mention.span))) coveredWords.insert(word);
    const std::set<std::string> scopeWords{"системе", "доме", "сессии", "работе", "устройстве", "устройствах"};
    const std::set<std::string> temporal{"данный момент", "настоящий момент", "последний раз", "этот раз", "самом деле"};
    for (std::size_t i=0;i+1<w.size();++i) if (w[i]=="на"||w[i]=="в"||w[i]=="у") {
        const auto phrase=w[i+1]+(i+2<w.size()?" "+w[i+2]:"");
        if (!coveredWords.count(w[i+1]) && !scopeWords.count(w[i+1]) && !temporal.count(phrase)) {
            reply.text = "Не удалось однозначно определить названную зону или устройство. Уточните название."; reply.reason = "unknown_scope"; return reply;
        }
    }
    if (kind == Question::LastAction && targets.empty() && has(w, {"включила", "выключила", "отключила"})) {
        reply.text = "Для какого устройства нужно проверить команду? Назовите устройство и зону."; reply.reason = "target_missing"; return reply;
    }
    if (kind == Question::Capabilities) {
        reply.text = "Я на связи. В активной конфигурации устройств: " + std::to_string(f.catalog->configuration().devices.size()) + ". Модель распознавания намерений " + (f.modelReady ? "готова." : "недоступна.");
        if (detailed || has(w, {"умеешь", "возможности"})) reply.text += " Могу выполнить разрешённую команду, назвать известное состояние, последнее действие и причину отказа. Ожидаемое состояние отличаю от подтверждённого. Общение на другие темы пока не поддерживается.";
    } else if (kind == Question::Failure) {
        const Event* latest = nullptr;
        for (auto it=f.history.rbegin();it!=f.history.rend();++it) if (production(*it) &&
            (targets.empty() || std::find(targets.begin(),targets.end(),it->execution.targetId)!=targets.end())) { latest=&*it;break; }
        const bool own = attempt_ && (targets.empty() || (attempt_->execution &&
            std::find(targets.begin(),targets.end(),attempt_->execution->targetId)!=targets.end()));
        if (latest && (!own || latest->sequence > attempt_->seenSequence)) {
            if (latest->generation != f.generation) reply.text = "Последняя команда относится к прежней конфигурации. Её результат нельзя переносить на текущую.";
            else reply.text = latest->execution.success ? "Последняя команда принята исполнителем. Это не подтверждает физическое состояние." : failure(latest->execution.error);
        } else if (!own) reply.text = "В этой сессии ещё нет результата команды для выбранного устройства. После перезапуска прежние результаты не сохраняются.";
        else if (attempt_->hash != f.catalog->contentHash()) reply.text = "Результат последней голосовой команды относится к прежней конфигурации. Повторного исполнения не было.";
        else if (attempt_->execution && attempt_->execution->success) reply.text = "Последняя голосовая команда принята исполнителем. Это не подтверждает физическое состояние устройства.";
        else reply.text = attempt_->execution ? failure(attempt_->execution->error) : failure(attempt_->rejection);
    } else if (kind == Question::LastAction) {
        auto found = f.history.rend();
        for (auto it = f.history.rbegin(); it != f.history.rend(); ++it) if (production(*it) && (targets.empty() || std::find(targets.begin(), targets.end(), it->execution.targetId) != targets.end())) { found = it; break; }
        if (found == f.history.rend()) reply.text = "В этой сессии нет записей исполнения для выбранного устройства. Проверки в Playground не считаю действиями оборудования.";
        else if (found->generation != f.generation) reply.text = "Последнее действие относится к прежней версии конфигурации. По нему нельзя определить текущее состояние.";
        else reply.text = executionText(f, found->execution, detailed);
    } else if (kind == Question::OnList) {
        std::vector<std::string> confirmed, predicted, reportedTargets; int unknown = 0;
        for (const auto& d : f.catalog->configuration().devices) {
            if (!targets.empty() && std::find(targets.begin(), targets.end(), d.id) == targets.end()) continue;
            const auto p = powered(f, d);
            if (!p) { ++unknown; continue; }
            if (!p->second) continue;
            (p->first == StateKnowledge::Confirmed ? confirmed : predicted).push_back(bounded(d.name));
            reportedTargets.push_back(d.id);
        }
        const auto list = [detailed](const std::vector<std::string>& names) { std::string result; std::size_t n = 0; for (const auto& name : names) { if (n++ == (detailed ? 6u : 3u)) break; result += (result.empty() ? "" : ", ") + name; } if (n > (detailed ? 6u : 3u)) result += "; полный список — в Web Admin"; return result; };
        reply.text = confirmed.empty() ? "Нет свежих подтверждённых данных о включённых устройствах." : "По свежим подтверждённым наблюдениям включены: " + list(confirmed) + ".";
        if (!predicted.empty()) reply.text += " Ожидаются включёнными, без физического подтверждения: " + list(predicted) + ".";
        if (reportedTargets.size() == 1) referent = reportedTargets.front();
        if (unknown) reply.text += " Неизвестное или устаревшее состояние у устройств: " + std::to_string(unknown) + ".";
    } else {
        if (targets.empty()) { reply.text = "Назовите устройство или зону, состояние которых нужно проверить."; reply.reason = "target_missing"; return reply; }
        const std::size_t limit = detailed ? 4 : 2;
        for (std::size_t i = 0; i < targets.size() && i < limit; ++i) if (const auto* d = device(f, targets[i])) reply.text += stateText(f, *d, detailed) + " ";
        if (targets.size() > limit) reply.text += "Остальные устройства — в Web Admin.";
    }
    const bool newSubject = !follow || !grounding.devices.empty() || !grounding.locations.empty();
    context_ = {f.catalog->contentHash(), targets.size() == 1 ? targets.front() : referent,
        locations.size() == 1 ? locations.front() : std::string{},
        newSubject || context_.question == Question::None ? f.steadyMs : context_.at,
        newSubject ? 1 : context_.turns + 1, kind};
    reply.text = bounded(reply.text, 1000);
    reply.reason = "facts_only";
    return reply;
}
}
