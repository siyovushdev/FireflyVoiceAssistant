#include "firefly/dialogue/OperationalDialogue.h"
#include "firefly/runtime/DomainView.h"
#include "firefly/llm/PowerIntent.h"
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
const std::set<std::string>& onPredicates() {
    static const std::set<std::string> values{"работает", "работают", "горит", "горят",
        "включен", "включён", "включена", "включено", "включены"};
    return values;
}
const std::set<std::string>& statePredicates() {
    static const auto values = [] {
        auto result = onPredicates();
        for (const auto* word : {"выключен", "выключён", "выключена", "выключено", "выключены",
                                "отключен", "отключён", "отключена", "отключено", "отключены"}) result.insert(word);
        return result;
    }();
    return values;
}
bool any(const std::vector<std::string>& words, const std::set<std::string>& values) {
    return std::any_of(words.begin(), words.end(), [&](const auto& word) { return values.count(word); });
}
bool questionCue(const std::vector<std::string>& words) {
    if (has(words, {"онлайн", "оффлайн", "доступен", "доступно", "связь", "связи"})) return true;
    if (has(words, {"почему", "зачем", "что", "где", "как", "когда", "какие", "какое", "какая", "какой", "сколько", "ли", "состояние", "статус"})) return true;
    if (has(words, {"пусть", "пускай"})) return false;
    return any(words, statePredicates()) ||
        (has(words, {"ты", "ассистент"}) && has(words, {"работаешь", "готова", "доступна", "умеешь"})) ||
        (has(words, {"ты", "вы"}) && has(words, {"сделала", "сделал", "включила", "выключила", "отключила", "смогла", "выполнила", "приняла"}));
}
std::vector<std::string> ungroundedWords(std::string_view original, const runtime::Grounding& grounding) {
    // Coverage analysis only. The original input passed to the command engine
    // and SafetyGate is never rewritten. Room membership is not name evidence.
    auto uncovered = detail::lowercase(original);
    for (const auto& m : grounding.mentions) if ((m.category == "device" || m.category == "location") &&
        m.begin <= m.end && m.end <= uncovered.size()) std::fill(uncovered.begin() + m.begin, uncovered.begin() + m.end, ' ');
    return detail::words(uncovered);
}
bool coveredQuestion(const std::vector<std::string>& uncovered) {
    // Bounded question grammar, not a list of device names. Unknown nouns or
    // qualifiers require clarification instead of selecting other room devices.
    static const auto frame = [] {
        auto words = statePredicates();
        for (const auto* word : {"а", "в", "во", "на", "у", "из", "о", "об", "для", "со", "с", "и", "или",
            "ты", "вы", "я", "мы", "мне", "нас", "он", "она", "оно", "его", "ее", "её", "это", "там", "тут", "здесь",
            "что", "где", "как", "когда", "какие", "какое", "какая", "какой", "сколько", "ли", "почему", "зачем", "разве",
            "сейчас", "уже", "еще", "ещё", "тоже", "точно", "теперь", "же", "не", "нет", "пожалуйста",
            "скажи", "подскажи", "расскажи", "объясни", "сообщи", "можете", "бы",
            "состояние", "статус", "яркость", "температура", "положение", "доступность", "связь", "связи", "онлайн", "оффлайн", "доступен", "доступно", "есть", "сети",
            "команда", "команды", "команду", "действие", "действия", "действий", "устройства", "устройств", "устройстве", "устройствах", "приборы", "оборудование",
            "последним", "последнее", "последнюю", "последней", "последний", "последняя", "предыдущая", "сделала", "сделал",
            "включила", "включил", "выключила", "выключил", "отключила", "отключил", "смогла", "выполнила", "выполнилась", "выполнилось", "приняла",
            "получилось", "сработало", "сработал", "сработала", "прошёл", "прошел", "прошла", "запуск", "ошибка", "ошибки", "ошибку", "отказ", "отказа",
            "устройству", "работает", "работаешь", "работу", "работе", "готова", "доступна", "умеешь", "можешь", "возможности", "ассистент",
            "данный", "настоящий", "момент", "этот", "раз", "самом", "деле", "системе", "доме", "сессии"}) words.insert(word);
        return words;
    }();
    return std::all_of(uncovered.begin(), uncovered.end(), [&](const auto& word) { return frame.count(word); });
}
std::string rejectedTarget(const Facts& facts, std::string_view original) {
    if (original.empty()) return {};
    const auto grounding = runtime::DomainView(facts.catalog).resolve(original, false);
    if (grounding.ambiguous || grounding.devices.size() != 1) return {};
    auto words = ungroundedWords(original, grounding);
    if (!words.empty() && words.front() == "а") words.erase(words.begin());
    words.erase(std::remove_if(words.begin(), words.end(), [](const auto& word) {
        return word == "не" || word == "нельзя" || word == "никогда";
    }), words.end());
    std::string intent;
    for (const auto& word : words) intent += (intent.empty() ? "" : " ") + word;
    // Reuse the existing request-form analysis. Unknown object/location words,
    // multiple clauses and statements cannot establish a rejected target.
    return llm::powerRequestForm(intent) ? grounding.devices.front() : std::string{};
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
std::string label(const Facts& f, const config::Device& d) {
    std::string result = bounded(d.name);
    if (d.roomId) for (const auto& room : f.catalog->configuration().rooms) if (room.id == *d.roomId && !room.locations.empty()) {
        result += " " + bounded(room.locations.front()); break;
    }
    return result;
}
bool failedRead(const Facts& f, const std::string& id) {
    return std::find(f.failedReads.begin(), f.failedReads.end(), id) != f.failedReads.end();
}
bool fresh(const Facts& f, const execution::PropertyKnowledge& p) {
    return p.value && p.knowledge == StateKnowledge::Confirmed && !execution::isStale(p, f.steadyMs) &&
        p.freshnessSteadyMs && *p.freshnessSteadyMs <= f.steadyMs && p.orderSteadyMs && *p.orderSteadyMs <= f.steadyMs;
}
std::string historicalPower(const Facts& f, const config::Device& d) {
    const auto* known = state(f, d.id);
    if (known) for (const auto* key : {"power", "on", "running", "brightness"}) {
        const auto at = known->properties.find(key);
        if (at == known->properties.end()) continue;
        const auto& p = at->second;
        if (!p.lastObservedValue || !p.lastObservationSteadyMs || *p.lastObservationSteadyMs > f.steadyMs) continue;
        const auto& v = p.lastObservedValue->data();
        std::optional<bool> on;
        if (std::holds_alternative<bool>(v) && std::string(key) != "brightness") on = std::get<bool>(v);
        else if (std::string(key) == "brightness" && d.kind == config::DeviceKind::Light && std::holds_alternative<std::int64_t>(v)) on = std::get<std::int64_t>(v) > 0;
        if (on) return std::string("Последнее показание: питание ") + (*on ? "включено." : "выключено.") + " Сейчас не могу проверить.";
    }
    return "Сейчас не могу проверить.";
}
std::string stateText(const Facts& f, const config::Device& d, bool detailed) {
    auto result = label(f,d) + ": ";
    const auto* known = state(f,d.id);
    if (failedRead(f,d.id) || !known || known->properties.empty()) return result + "сейчас не могу проверить.";
    int count = 0;
    for (const auto& [id,p] : known->properties) {
        if (++count > (detailed ? 5 : 2)) break;
        std::string name = id;
        for (const auto& field : d.state) if (field.id == id) name = field.name;
        if (id == "brightness" && d.kind == config::DeviceKind::Light) name = "Яркость";
        if (id == "position" && d.kind == config::DeviceKind::EntranceDoor) name = "Положение";
        result += bounded(name) + ": ";
        if (!fresh(f,p)) {
            if (p.lastObservedValue && p.lastObservationSteadyMs && *p.lastObservationSteadyMs <= f.steadyMs)
                result += "последнее показание «" + scalar(*p.lastObservedValue) + "», сейчас не могу проверить. ";
            else result += "сейчас не могу проверить. ";
            continue;
        }
        const bool power = id == "power" || id == "on" || id == "running";
        if (power && std::holds_alternative<bool>(p.value->data())) result += std::get<bool>(p.value->data()) ? "включено. " : "выключено. ";
        else result += scalar(*p.value) + ". ";
    }
    return result;
}
std::string failure(const std::string& error) {
    const auto reason = error.substr(0,error.find(':'));
    if (reason == "binding_missing" || reason == "binding_ambiguous") return "Для этого действия не настроено подключение.";
    if (reason == "connection_disabled" || reason == "unsupported_executor" || reason == "mqtt_unavailable" || reason == "mqtt_connection_missing" || reason == "connection_failure")
        return "Не удалось связаться с устройством.";
    if (reason == "ambiguous") return "Уточни устройство и команду.";
    if (reason == "grounding" || reason == "domain" || reason == "unsupported" || reason == "safety" || reason == "model_unsupported" || reason == "command_guard" || reason == "decoder" || reason == "runtime")
        return "Не могу выполнить эту команду.";
    if (reason == "recognition") return "Повтори, пожалуйста.";
    // Delivery may have happened; neither a denial nor an automatic retry.
    return "Не удалось подтвердить выполнение.";
}
std::string executionText(const Facts& f, const execution::ExecutionResult& e, bool) {
    if (!e.success || e.status != execution::ExecutionStatus::Accepted) return failure(e.error);
    const auto* d = device(f,e.targetId);
    if (!d) return "Это устройство уже не в текущих настройках.";
    std::string action = "действие";
    for (const auto& a : f.catalog->configuration().actions) if (a.id == e.actionId) action = a.name;
    return "Отправила команду «" + bounded(action) + "» для «" + label(f,*d) + "».";
}
bool production(const Event& e) { return e.source == "voice" || e.source == "admin-production" || e.source == "admin-device"; }
std::optional<std::pair<StateKnowledge, bool>> powered(const Facts& f, const config::Device& d) {
    const auto* s = state(f, d.id); if (!s) return {};
    for (const auto& key : {"power", "on", "running", "brightness"}) {
        const auto p = s->properties.find(key);
        if (p == s->properties.end() || !p->second.value || p->second.knowledge == StateKnowledge::Unknown || execution::isStale(p->second, f.steadyMs)) continue;
        if (p->second.knowledge == StateKnowledge::Confirmed && !fresh(f,p->second)) continue;
        const auto& v = p->second.value->data();
        if (std::holds_alternative<bool>(v) && std::string(key) != "brightness") return std::make_pair(p->second.knowledge, std::get<bool>(v));
        if (std::string(key) == "brightness" && d.kind == config::DeviceKind::Light && std::holds_alternative<std::int64_t>(v)) return std::make_pair(p->second.knowledge, std::get<std::int64_t>(v) > 0);
    }
    return {};
}
}
bool OperationalDialogue::questionLike(std::string_view original) {
    const auto w = detail::words(detail::lowercase(original));
    if (questionCue(w)) return true;
    if (has(w, {"пусть", "пускай"})) return false;
    const std::set<std::string> continuation{"на", "в", "там", "он", "она", "оно", "это"};
    return w.size() > 1 && w.front() == "а" && continuation.count(w[1]);
}
void OperationalDialogue::clear() { std::lock_guard<std::mutex> lock(mutex_); context_ = {}; attempt_.reset(); }
void OperationalDialogue::rememberAttempt(const Facts& f, const std::optional<execution::ExecutionResult>& e, const std::string& rejection, std::string_view original, std::string feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t seen = 0;
    for (const auto& event : f.history) if (production(event)) seen = std::max(seen, event.sequence);
    attempt_ = Attempt{f.catalog->contentHash(), rejection, e ? e->targetId : rejectedTarget(f, original), e, seen, std::move(feedback)};
    context_ = {};
    if (e && device(f, e->targetId)) context_ = {f.catalog->contentHash(), e->targetId, {}, f.steadyMs, 0, Question::State};
}
bool OperationalDialogue::executionConfirmed(const Facts& f, const execution::ExecutionResult& e,
                                                    const actions::GenericAction& command, std::int64_t after) {
    if (!e.success || e.status != execution::ExecutionStatus::Accepted || !e.current ||
        e.targetId != command.targetId() || e.actionId != command.actionId() || failedRead(f,e.targetId)) return false;
    const auto* target=device(f,e.targetId);
    const auto definition=f.catalog->actionIndex().find(e.actionId);
    if(!target || !target->enabled || definition==f.catalog->actionIndex().end())return false;
    const auto& action=f.catalog->configuration().actions[definition->second];
    if(!action.enabled || std::find(target->actionIds.begin(),target->actionIds.end(),e.actionId)==target->actionIds.end())return false;
    if ((target->kind==config::DeviceKind::Generic && !std::holds_alternative<execution::TypedState>(*e.current)) ||
        (target->kind==config::DeviceKind::Light && !std::holds_alternative<execution::LightState>(*e.current)) ||
        (target->kind==config::DeviceKind::EntranceDoor && !std::holds_alternative<execution::DoorState>(*e.current)))return false;
    std::map<std::string,actions::Scalar> expected;
    if (const auto* light = std::get_if<execution::LightState>(&*e.current)) expected.emplace("brightness",actions::Scalar(light->brightness));
    else if (const auto* door = std::get_if<execution::DoorState>(&*e.current)) expected.emplace("position",actions::Scalar(std::string(door->position == execution::DoorPosition::Open ? "OPEN" : "CLOSED")));
    else if (std::holds_alternative<execution::TypedState>(*e.current)) {
        const auto at = f.catalog->actionIndex().find(e.actionId);
        if (at == f.catalog->actionIndex().end()) return false;
        for (const auto& assignment : f.catalog->configuration().actions[at->second].assignments) {
            if (assignment.argument) {
                const auto argument=command.arguments().find(*assignment.argument);
                if(argument==command.arguments().end())continue;
                expected.emplace(assignment.field,argument->second);
            } else if(assignment.value)expected.emplace(assignment.field,*assignment.value);
            else return false;
        }
    }
    const auto* known = state(f,e.targetId);
    if (expected.empty() || !known || after > f.steadyMs) return false;
    for (const auto& [field,value] : expected) {
        const auto at = known->properties.find(field);
        if (at == known->properties.end() || !fresh(f,at->second) || *at->second.value != value ||
            !at->second.orderSteadyMs || *at->second.orderSteadyMs > f.steadyMs || *at->second.orderSteadyMs < after || *at->second.freshnessSteadyMs < after) return false;
    }
    return true;
}
std::string OperationalDialogue::commandReply(const Facts& f, const std::optional<execution::ExecutionResult>& e,
                                            const std::string& rejection, bool, std::optional<std::int64_t> after, const actions::GenericAction* command) {
    if (!e || !e->success || e->status != execution::ExecutionStatus::Accepted) return failure(e ? e->error : rejection);
    if (after && command && executionConfirmed(f,*e,*command,*after)) return {};
    const auto* known = state(f,e->targetId);
    if (known && after && !failedRead(f,e->targetId)) for (const auto& [field,p] : known->properties) {
        (void)field;
        if (fresh(f,p) && p.orderSteadyMs && *p.orderSteadyMs >= *after && *p.freshnessSteadyMs >= *after)
            return "Устройство ответило, но выполнение не подтвердилось.";
    }
    return "Команду отправила, ответа пока нет.";
}
Reply OperationalDialogue::ask(std::string_view original, const Facts& input, bool enabled, bool detailed,
                              const Classifier& classify, const Refresh& refresh) {
    auto f = input;
    if (!questionLike(original)) return {};
    if (original.size() > 1024) {
        std::lock_guard<std::mutex> lock(mutex_); context_ = {};
        return {true, "Задай один короткий вопрос.", "input_limit", Question::Unclear};
    }
    runtime::DomainView domain(f.catalog);
    const auto grounding = domain.resolve(original, false);
    const auto uncovered = ungroundedWords(original, grounding);
    const auto w = detail::words(detail::lowercase(original));
    if (!questionCue(uncovered)) {
        static const std::set<std::string> ellipsis{"а", "на", "в", "во", "у", "там", "тут", "здесь", "он", "она", "оно", "это", "сейчас", "уже", "ещё", "еще", "тоже", "пожалуйста"};
        if (!std::all_of(uncovered.begin(), uncovered.end(), [&](const auto& word) { return ellipsis.count(word); })) return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Reply reply{true, {}, {}, Question::Unclear};
    if (!enabled) { context_ = {}; reply.text = "Вопросы об устройствах выключены."; reply.reason = "questions_disabled"; return reply; }
    if (context_.hash != f.catalog->contentHash() || f.steadyMs < context_.at || f.steadyMs - context_.at > 120000 || context_.turns >= 6) context_ = {};
    std::string referent;
    Question kind = Question::Unclear;
    if (has(uncovered, {"почему", "ошибка", "ошибки", "ошибку", "отказ", "отказа"})) kind = Question::Failure;
    else if (has(uncovered,{"ты","вы"}) && has(uncovered,{"включила","выключила","отключила"}) && !has(uncovered,{"последним","последнее","последнюю","последний","когда"})) kind=Question::State;
    else if (has(uncovered, {"последним", "последнее", "последнюю", "сделала", "сделал", "включила", "выключила", "отключила", "выполнила", "приняла"})) kind = Question::LastAction;
    else if ((has(uncovered, {"умеешь", "можешь", "возможности"}) && has(uncovered, {"что", "как"})) || (has(uncovered, {"ты", "ассистент"}) && has(uncovered, {"работаешь", "готова", "доступна"}))) kind = Question::Capabilities;
    else if (has(uncovered, {"что", "какие"}) && any(uncovered, onPredicates())) kind = Question::OnList;
    else if (any(uncovered, statePredicates()) || has(uncovered, {"состояние", "статус", "яркость", "температура", "положение", "онлайн", "оффлайн", "доступен", "доступно", "доступность", "связь", "связи"})) kind = Question::State;
    const bool follow = w.size() > 1 && w.front() == "а";
    if (kind == Question::Unclear && follow) {
        if (context_.question == Question::State || context_.question == Question::OnList || context_.question == Question::LastAction) kind = context_.question;
        else { reply.text = "Что хочешь узнать об устройстве?"; reply.reason = "context_missing"; return reply; }
    }
    if (kind == Question::Unclear && classify) kind = classify(original);
    if (kind == Question::None || kind == Question::Unclear) {
        context_ = {};
        // Silently consume unsupported questions; never fall through to a command.
        reply.reason = "outside_operational_scope"; return reply;
    }
    reply.question = kind;
    std::vector<std::string> targets = grounding.devices;
    auto locations = grounding.locations;
    if (grounding.ambiguous || locations.size() > 1) {
        context_ = {};
        reply.text = "Уточни устройство и зону."; reply.reason = "ambiguous"; return reply;
    }
    if (!coveredQuestion(uncovered)) {
        context_ = {};
        reply.text = "Не нашла такое устройство или зону.";
        reply.reason = kind == Question::LastAction && targets.empty() && locations.empty() ? "target_missing" : "unknown_scope";
        return reply;
    }
    if (targets.empty() && locations.size() == 1) {
        for (const auto& d : f.catalog->configuration().devices) if (d.roomId == locations.front()) targets.push_back(d.id);
    }
    if (targets.empty() && locations.empty() && has(w, {"он", "она", "оно", "его", "ее", "её", "это"})) {
        const std::set<std::string> allowed{"а", "он", "она", "оно", "его", "ее", "её", "это", "сейчас", "работает", "горит", "включен", "включён", "выключен", "выключён", "ли", "уже", "еще", "ещё", "точно", "теперь", "не", "почему", "включается", "выключается", "включился", "выключился", "ответил", "ответила", "заработал"};
        const bool clean = std::all_of(w.begin(), w.end(), [&](const auto& word) { return allowed.count(word) || statePredicates().count(word); });
        if (clean && !context_.target.empty() && device(f, context_.target)) targets.push_back(context_.target);
        else { context_ = {}; reply.text = "Не знаю, о каком устройстве речь."; reply.reason = "context_missing"; return reply; }
    }
    if (locations.size() == 1 && targets.empty()) {
        context_ = {};
        reply.text = "В этой зоне нет настроенных устройств."; reply.reason = "empty_location"; return reply;
    }
    if (kind == Question::LastAction && targets.empty() && has(w, {"включила", "выключила", "отключила"})) {
        context_ = {};
        reply.text = "О каком устройстве спрашиваешь?"; reply.reason = "target_missing"; return reply;
    }
    // Only after exact grounding/scope validation may an owner perform a
    // bounded read. Unknown entities, commands and disabled questions never do.
    if (refresh && (kind == Question::State || kind == Question::OnList)) {
        auto requested = targets;
        if (requested.empty() && kind == Question::OnList) for (const auto& d : f.catalog->configuration().devices) if (d.enabled) requested.push_back(d.id);
        if (requested.size() > (detailed ? 4u : 2u)) requested.resize(detailed ? 4 : 2);
        const auto updated = refresh(requested);
        if (updated.generation != f.generation || !updated.catalog || updated.catalog->contentHash() != f.catalog->contentHash()) {
            context_ = {};return {true,"Настройки изменились. Повтори вопрос.","generation_changed",kind};
        }
        f = updated;
    }
    if (kind == Question::Capabilities) {
        // Owner preference: no generic readiness/capability speech. Keep the
        // question handled so silence cannot fall through into command execution.
        context_ = {};
        reply.reason = "capabilities_silent";
        return reply;
    } else if (kind == Question::Failure) {
        const Event* latest = nullptr;
        for (auto it=f.history.rbegin();it!=f.history.rend();++it) if (production(*it) &&
            (targets.empty() || std::find(targets.begin(),targets.end(),it->execution.targetId)!=targets.end())) { latest=&*it;break; }
        const bool own = attempt_ && (targets.empty() || (!attempt_->target.empty() &&
            std::find(targets.begin(),targets.end(),attempt_->target)!=targets.end()));
        const bool uncertain = attempt_ && !attempt_->execution && attempt_->target.empty() && !targets.empty();
        if (latest && ((!own && !uncertain) || latest->sequence > attempt_->seenSequence)) {
            if (latest->generation != f.generation) reply.text = "Это было до изменения настроек. Текущий результат неизвестен.";
            else reply.text = latest->execution.success ? "Команду отправила." : failure(latest->execution.error);
        } else if (!own && !uncertain) reply.text = "В этой сессии ещё не было команды для этого устройства.";
        else if (attempt_->hash != f.catalog->contentHash()) reply.text = "Это было до изменения настроек. Текущий результат неизвестен.";
        else if (uncertain) reply.text = "Не знаю, к какому устройству относилась последняя просьба. " + failure(attempt_->rejection);
        else if (attempt_->execution && attempt_->execution->success) reply.text = attempt_->feedback.empty() ? "Команду отправила." : attempt_->feedback;
        else reply.text = attempt_->execution ? failure(attempt_->execution->error) : failure(attempt_->rejection);
    } else if (kind == Question::LastAction) {
        auto found = f.history.rend();
        for (auto it = f.history.rbegin(); it != f.history.rend(); ++it) if (production(*it) && (targets.empty() || std::find(targets.begin(), targets.end(), it->execution.targetId) != targets.end())) { found = it; break; }
        if (found == f.history.rend()) reply.text = "В этой сессии я ещё не отправляла команды.";
        else if (found->generation != f.generation) reply.text = "Это было до изменения настроек. Текущий результат неизвестен.";
        else {
            reply.text = executionText(f, found->execution, detailed);
            if (device(f, found->execution.targetId)) referent = found->execution.targetId;
        }
    } else if (kind == Question::OnList) {
        std::vector<std::string> confirmed, considered; int unknown = 0;
        for (const auto& d : f.catalog->configuration().devices) {
            if (!d.enabled || (!targets.empty() && std::find(targets.begin(),targets.end(),d.id)==targets.end())) continue;
            considered.push_back(d.id);
            const auto p = powered(f,d);
            if (failedRead(f,d.id) || !p || p->first != StateKnowledge::Confirmed) {++unknown;continue;}
            if (p->second) confirmed.push_back(label(f,d));
        }
        if (considered.size()==1) referent=considered.front();
        if (confirmed.empty()) {
            if (unknown && considered.size()==1) reply.text=label(f,*device(f,considered.front()))+": сейчас не могу проверить.";
            else reply.text=unknown ? "Не могу проверить все устройства." : "Сейчас всё выключено.";
        } else {
            reply.text="Включены: ";std::size_t n=0;
            for(const auto& name:confirmed){if(n==(detailed?6u:3u))break;reply.text+=(n++?", ":"")+name;}
            reply.text+=".";if(unknown)reply.text+=" Про остальные пока не знаю.";
        }
    } else {
        if (targets.empty()) { context_={};reply.text="О каком устройстве спрашиваешь?";reply.reason="target_missing";return reply; }
        // A one-device yes/no answer needs no grammatical guess about arbitrary
        // configured names. Negative/alternative questions report the value.
        const auto* d=targets.size()==1?device(f,targets.front()):nullptr;
        const auto p=d?powered(f,*d):std::nullopt;
        const bool propertyQuestion=has(uncovered,{"яркость","температура","положение"});
        const bool powerDevice=d && (d->kind==config::DeviceKind::Light || std::any_of(d->state.begin(),d->state.end(),[](const auto& field){return (field.id=="power"||field.id=="on"||field.id=="running")&&field.type=="boolean";}));
        const bool availabilityQuestion = has(uncovered,{"онлайн","оффлайн","доступен","доступно","доступность","связь","связи"});
        if (d && availabilityQuestion) {
            const auto* known = state(f,d->id);
            const std::string online = known ? execution::availability(*known,f.steadyMs) : "unknown";
            reply.text = online == "online" && !failedRead(f,d->id) ? "Устройство на связи." :
                online == "offline" || failedRead(f,d->id) ? "Сейчас нет связи с устройством." : "Данных от устройства ещё нет.";
        }
        else if (d && powerDevice && !detailed && !propertyQuestion && (!p || failedRead(f,d->id) || p->first!=StateKnowledge::Confirmed)) reply.text=historicalPower(f,*d);
        else if (d && powerDevice && !detailed && !propertyQuestion && p) {
            const bool on=any(uncovered,onPredicates());
            const bool off=any(uncovered,statePredicates())&&!on;
            if ((on||off) && !has(uncovered,{"не","или"})) reply.text=(on?p->second:!p->second)?"Да.":"Нет.";
            else reply.text=p->second?"Питание включено.":"Питание выключено.";
        } else {
            const std::size_t limit=detailed?4:2;
            for(std::size_t i=0;i<targets.size()&&i<limit;++i)if(const auto* current=device(f,targets[i]))reply.text+=stateText(f,*current,detailed)+" ";
            if(targets.size()>limit)reply.text+="Остальные устройства можно посмотреть в приложении.";
        }
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
