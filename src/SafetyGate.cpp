#include "firefly/language/Integer.h"
#include "firefly/SafetyGate.h"
#include "firefly/llm/PowerIntent.h"
#include "Text.h"
#include <algorithm>
#include <regex>
#include <vector>
#include <sstream>
#include <iomanip>
#include <locale>
#include <charconv>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <set>
#include <utility>

namespace firefly {
namespace {
bool lightWord(std::string_view word) {
    return word == "свет" || word == "света" || word == "освещение" ||
           word == "освещения" || word == "яркость" || word == "яркости";
}
bool onWord(std::string_view word) {
    return word == "включи" || word == "включите" || word == "включить" ||
           word == "включай" || word == "включайте";
}
bool offWord(std::string_view word) {
    return word == "выключи" || word == "выключите" || word == "выключить" ||
           word == "выключай" || word == "выключайте";
}
bool percentWord(std::string_view word) {
    return word == "%" || word == "процент" || word == "процента" || word == "процентов";
}
int cardinal(std::string_view word) {
    // A bounded safety vocabulary, not a parser route or transcript conversion.
    static constexpr std::string_view small[] = {"ноль", "один", "два", "три", "четыре",
        "пять", "шесть", "семь", "восемь", "девять", "десять", "одиннадцать",
        "двенадцать", "тринадцать", "четырнадцать", "пятнадцать", "шестнадцать",
        "семнадцать", "восемнадцать", "девятнадцать"};
    for (int value = 0; value < 20; ++value) if (word == small[value]) return value;
    static constexpr std::string_view tens[] = {"двадцать", "тридцать", "сорок", "пятьдесят",
        "шестьдесят", "семьдесят", "восемьдесят", "девяносто", "сто"};
    for (int index = 0; index < 9; ++index) if (word == tens[index]) return (index + 2) * 10;
    return word == "нуль" ? 0 : -1;
}
struct LightState {
    int minimum = 0;
    int maximum = 100;
    bool specified = false;
    void require(int low, int high) {
        specified = true;
        minimum = std::max(minimum, low);
        maximum = std::min(maximum, high);
    }
    bool impossible() const { return minimum > maximum; }
};
LightState requestedLightState(std::string_view original, const std::vector<std::string>& words) {
    LightState state;
    if (std::none_of(words.begin(), words.end(), lightWord)) return state;
    bool on = false;
    bool amount = false;
    for (const auto& word : words) {
        if (offWord(word)) state.require(0, 0);
        if (onWord(word)) { state.require(1, 100); on = true; }
        if (word == "максимум") { state.require(100, 100); amount = true; }
        if (word == "наполовину") { state.require(50, 50); amount = true; }
    }
    if (on && std::find(words.begin(), words.end(), "полностью") != words.end()) {
        state.require(100, 100);
        amount = true;
    }
    const auto reject = [] { return LightState{1, 0}; };
    // words() splits decimal punctuation. Never reinterpret a fractional state
    // requirement as separate integers (especially 0.0 as an OFF repair).
    static const std::regex fraction("[0-9][.,][0-9]");
    if (std::regex_search(original.begin(), original.end(), fraction)) return reject();
    bool percentageConsumed = false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (percentWord(words[i])) {
            if (!percentageConsumed) return reject(); // Unresolved explicit value.
            percentageConsumed = false;
            continue;
        }
        percentageConsumed = false;
        std::string_view token = words[i];
        bool attachedPercent = false;
        for (const auto suffix : {std::string_view("%"), std::string_view("процентов"),
                                 std::string_view("процента"), std::string_view("процент")}) {
            if (token.size() > suffix.size() && token.substr(token.size() - suffix.size()) == suffix) {
                token.remove_suffix(suffix.size());
                attachedPercent = true;
                break;
            }
        }
        int value = cardinal(token);
        const bool numeric = !token.empty() && ((token.front() >= '0' && token.front() <= '9') ||
                                                token.front() == '-' || token.front() == '+');
        if (numeric) {
            value = 0;
            for (const char digit : token) {
                if (digit < '0' || digit > '9' || value > 100) { value = -1; break; }
                value = value * 10 + digit - '0';
            }
        }
        std::size_t last = i;
        if (!numeric && !attachedPercent && value >= 20 && value <= 90 && i + 1 < words.size()) {
            const int unit = cardinal(words[i + 1]);
            if (unit > 0 && unit < 10) { value += unit; ++last; }
        }
        const bool unitFollows = last + 1 < words.size() && percentWord(words[last + 1]);
        const auto previous = i == 0 ? std::string_view{} : std::string_view(words[i - 1]);
        // Percentages are explicit. A bare value needs the existing brightness
        // slot ("на N", light/state word, or terminal value), not an unrelated unit.
        const bool bare = (numeric || value >= 0) &&
            (last + 1 == words.size() || previous == "на" || lightWord(previous) ||
             previous == "максимум" || previous == "наполовину");
        if (!attachedPercent && !unitFollows && !bare) continue;
        if (value < 0 || value > 100 || previous == "минус") return reject();
        state.require(value, value);
        amount = true;
        percentageConsumed = unitFollows;
        i = last;
    }
    // An unqualified ON has the established value 100; a model cannot invent
    // another positive level. Numeric-only requests are checked independently
    // of the model, including requests with an unrecognized intent/unit spelling.
    if (on && !amount) state.require(100, 100);
    return state;
}

using Span = std::pair<std::size_t, std::size_t>;

// Authorization accounts for the ORIGINAL request, not just entities the model
// happened to select. Unknown words can carry a location/other constraint; they
// must not disappear when an index miss falls back to inference. Arguments are
// covered only after the existing original-value checks below have admitted them.
bool requestCovered(std::string_view original, const runtime::ActionDescription& description,
                    const runtime::Grounding& grounding, const std::vector<Span>& arguments = {},
                    const std::vector<Span>& numericArguments = {}) {
    const auto& target = *description.target;
    if (grounding.ambiguous ||
        (!grounding.devices.empty() && (grounding.devices.size() != 1 || grounding.devices.front() != target.id)) ||
        (!grounding.locations.empty() && (!target.roomId || grounding.locations.size() != 1 ||
                                         grounding.locations.front() != *target.roomId))) return false;

    const auto text = detail::lowercase(original);
    const auto words = detail::words(original);
    std::vector<Span> spans;
    std::vector<bool> covered(words.size(), false);
    std::size_t offset = 0;
    for (const auto& word : words) {
        const auto begin = text.find(word, offset), end = begin + word.size();
        spans.emplace_back(begin, end);
        offset = end;
    }
    const auto cover = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = 0; i < spans.size(); ++i)
            if (spans[i].first >= begin && spans[i].second <= end) covered[i] = true;
    };
    for (const auto& mention : grounding.mentions)
        if ((mention.category == "device" && mention.entityId == target.id) ||
            (mention.category == "location" && target.roomId && mention.entityId == *target.roomId))
            cover(mention.begin, mention.end);
    for (const auto& span : arguments) cover(span.first, span.second);
    const auto phrase = [&](std::string_view alias) {
        const auto atom = detail::words(alias);
        if (atom.empty()) return;
        for (std::size_t i = 0; i + atom.size() <= words.size(); ++i)
            if (std::equal(atom.begin(), atom.end(), words.begin() + i))
                std::fill(covered.begin() + i, covered.begin() + i + atom.size(), true);
    };
    for (const auto& alias : description.definition->aliases) phrase(alias);
    phrase("будь добр");
    phrase("будьте добры");
    const auto operation = description.definition->operation;
    if (operation == config::Operation::SetBrightness) {
        // Established built-in light syntax. Values are independently checked by
        // requestedLightState; these words do not authorize a model-chosen value.
        for (std::size_t i = 0; i < words.size(); ++i)
            if (lightWord(words[i]) || onWord(words[i]) || offWord(words[i]) || percentWord(words[i]) ||
                words[i] == "поставь" || words[i] == "установи" || words[i] == "сделай" ||
                words[i] == "примерно" || words[i] == "максимум" || words[i] == "наполовину" ||
                language::integerAt(words, i)) covered[i] = true;
        if (std::any_of(words.begin(), words.end(), [](const auto& word) { return onWord(word) || offWord(word); }))
            for (std::size_t i = 0; i < words.size(); ++i)
                if (words[i] == "полностью") covered[i] = true;
    } else if (operation == config::Operation::OpenEntranceDoor) {
        phrase("на входе");
        for (std::size_t i = 0; i < words.size(); ++i)
            if (words[i] == "открой" || words[i] == "откройте" || words[i] == "открыть" ||
                words[i] == "открывай" || words[i] == "открывайте" ||
                words[i].rfind("двер", 0) == 0 || words[i].rfind("входн", 0) == 0) covered[i] = true;
    }
    for (const auto& parameter : description.definition->parameters)
        if (parameter.unit == "percent")
            for (std::size_t i = 0; i < words.size(); ++i)
                if (percentWord(words[i])) covered[i] = true;
    const auto quoted = [&](const Span& span) {
        return span.first > 0 && span.second < original.size() &&
            (original[span.first - 1] == '"' || original[span.first - 1] == '\'') &&
            original[span.second] == original[span.first - 1];
    };
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (words[i] != "в" && words[i] != "во" && words[i] != "на") continue;
        const auto insideArgument = std::any_of(arguments.begin(), arguments.end(), [&](const auto& span) {
            return quoted(span) && spans[i].first >= span.first && spans[i].second <= span.second;
        });
        if (insideArgument) continue; // Only an explicit quoted literal is opaque.
        if (i + 1 == words.size()) return false;
        const auto next = spans[i + 1];
        const auto entityObject = std::any_of(grounding.mentions.begin(), grounding.mentions.end(), [&](const auto& mention) {
            if (next.first < mention.begin || next.second > mention.end) return false;
            if (mention.category == "location") return target.roomId && mention.entityId == *target.roomId;
            // "на телевизоре" is a device object. A whole device/action alias
            // containing "... в <unknown place>" is NOT location authorization.
            return words[i] == "на" && mention.category == "device" && mention.entityId == target.id &&
                mention.begin >= spans[i].second;
        });
        const auto numericObject = std::any_of(numericArguments.begin(), numericArguments.end(), [&](const auto& span) {
            return next.first >= span.first && next.second <= span.second;
        });
        const auto quotedObject = std::any_of(arguments.begin(), arguments.end(), [&](const auto& span) {
            return quoted(span) && next.first >= span.first && next.second <= span.second;
        });
        const bool argumentObject = words[i] == "на" && (numericObject || quotedObject);
        const bool lightValue = words[i] == "на" && operation == config::Operation::SetBrightness &&
            (language::integerAt(words, i + 1) || words[i + 1] == "максимум" || words[i + 1] == "наполовину");
        const bool entrance = operation == config::Operation::OpenEntranceDoor && words[i] == "на" && words[i + 1] == "входе";
        if (!entityObject && !argumentObject && !lightValue && !entrance) return false;
    }
    for (std::size_t i = 0; i < words.size(); ++i) if (!covered[i]) {
        if (words[i] == "пожалуйста") continue;
        // A preposition alone is not evidence of a valid location. It needs a
        // covered entity/argument immediately after it (e.g. на телевизоре/40).
        if ((words[i] == "в" || words[i] == "во" || words[i] == "на") &&
            i + 1 < words.size() && covered[i + 1]) continue;
        return false;
    }
    return true;
}
}
actions::CandidateResult SafetyGate::applyPowerIntent(std::string_view original,
    const actions::CandidateResult& candidate, const runtime::DomainView& domain) {
    const auto scope = llm::powerIntentScope(domain, original);
    const auto* action = std::get_if<actions::GenericAction>(&candidate);
    if (!scope || !action || action->targetId() != scope->targetId || !action->arguments().empty())
        return actions::Unsupported{};
    if (action->actionId() != scope->offActionId && action->actionId() != scope->onActionId)
        return actions::Unsupported{};
    const bool on = action->actionId() == scope->onActionId;
    if (scope->explicitPower && on != *scope->explicitPower) return actions::Unsupported{};
    const auto description = domain.describe(*action);
    const auto grounding = domain.resolve(original, false);
    if (!description || !requestCovered(original, *description, grounding,
            {{scope->intentBegin, scope->intentEnd}})) return actions::Unsupported{};
    return domain.validate(candidate);
}
actions::CandidateResult SafetyGate::apply(std::string_view original, const actions::CandidateResult& candidate,
                                           const runtime::DomainView& domain) {
    const auto* action = std::get_if<actions::GenericAction>(&candidate);
    if (!action) return actions::Unsupported{};
    const auto description = domain.describe(*action);
    if (!description) return actions::Unsupported{};
    const auto words = detail::words(original);
    if (std::any_of(words.begin(), words.end(), [](const auto& word) {
        return word == "не" || word == "нет" || word == "ни" || word == "или" || word == "либо";
    })) return actions::Unsupported{};
    // Do not execute one branch of a coordinated request. A conjunction can be
    // part of a declared entity/action phrase only when the conservative parser
    // accounts for the entire request and yields this exact candidate.
    if (std::find(words.begin(), words.end(), "и") != words.end() &&
        domain.parseDeclared(original) != candidate) return actions::Unsupported{};
    const auto state = requestedLightState(original, words);
    // Contradictory ORIGINAL requirements reject every candidate, including a
    // guessed OFF value. This shared boundary never chooses an interpretation.
    if (state.impossible()) return actions::Unsupported{};
    const auto grounding = domain.resolve(original, true);
    if (description->definition->operation == config::Operation::OpenEntranceDoor) {
        const auto text = detail::lowercase(original);
        const bool openIntent = std::any_of(words.begin(), words.end(), [](const auto& word) {
            return word == "открой" || word == "откройте" || word == "открыть" ||
                   word == "открывай" || word == "открывайте";
        });
        bool entranceReference = text.find("входн") != std::string::npos;
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (words[i - 1] == "на" && words[i] == "входе") entranceReference = true;
        }
        // Require all three explicit cues in the original input. Never repair STT.
        if (!openIntent || !entranceReference ||
            text.find("двер") == std::string::npos) return actions::Unsupported{};
        return requestCovered(original, *description, grounding) ? candidate : actions::CandidateResult{actions::Unsupported{}};
    }
    if (grounding.ambiguous) return actions::Unsupported{};
    if (description->definition->operation == config::Operation::AssignState) {
        static const std::set<std::string> nonCommands{"как","почему","зачем","если","когда","потом","через","сначала","отмени","запрети","нельзя","запрещено","расскажи","скажи"};
        if(std::any_of(words.begin(),words.end(),[](const auto& word){return nonCommands.count(word);}))return actions::Unsupported{};
        if (grounding.devices.size() != 1 || grounding.devices.front() != action->targetId()) return actions::Unsupported{};
        const auto intents = domain.requestedActions(original, *description->target);
        if (intents.size() != 1 || intents.front() != action->actionId()) return actions::Unsupported{};
        struct Numeric { std::string text; std::size_t begin, end; bool consumed = false, numericArgument = false; };
        std::vector<Numeric> numbers;
        std::vector<Span> argumentSpans, numericArgumentSpans;
        const std::string source(original);
        static const std::regex numeric("[-+]?[0-9]+([.,][0-9]+)?([eE][-+]?[0-9]+)?");
        for (auto i=std::sregex_iterator(source.begin(),source.end(),numeric);i!=std::sregex_iterator();++i) {
            const auto begin=static_cast<std::size_t>(i->position()),end=begin+i->length();
            const bool entity = std::any_of(grounding.mentions.begin(),grounding.mentions.end(),[&](const auto& m){return begin>=m.begin&&end<=m.end;});
            if (!entity) numbers.push_back({i->str(),begin,end,false});
        }
        // Cardinal evidence remains tied to the ORIGINAL input, never to model output.
        const auto text = detail::lowercase(original);
        std::size_t wordOffset = 0;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto begin = text.find(words[i], wordOffset);
            wordOffset = begin + words[i].size();
            if (words[i].empty() || static_cast<unsigned char>(words[i][0]) < 128) continue;
            const auto cardinal = language::integerAt(words, i);
            if (!cardinal) continue;
            auto end = wordOffset;
            if (cardinal->count == 2) {
                const auto next = text.find(words[i + 1], end);
                if (std::string_view(text).substr(end, next - end).find_first_not_of(" \t\r\n") != std::string_view::npos)
                    return actions::Unsupported{};
                end = next + words[i + 1].size();
            }
            const bool entity = std::any_of(grounding.mentions.begin(), grounding.mentions.end(), [&](const auto& m) { return begin >= m.begin && end <= m.end; });
            if (!entity) {
                if (std::any_of(grounding.mentions.begin(), grounding.mentions.end(), [&](const auto& m) { return begin < m.end && end > m.begin; }))
                    return actions::Unsupported{};
                if (i && words[i - 1] == "минус") return actions::Unsupported{};
                numbers.push_back({std::to_string(cardinal->value), begin, end, false});
            }
            i += cardinal->count - 1;
            wordOffset = end;
        }
        const auto literalPresent = [&](const std::string& literal) {
            if (literal.empty()) return source.find("\"\"") != std::string::npos || source.find("''") != std::string::npos;
            const auto wordByte=[](unsigned char c){return c>=128||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';};
            for (auto pos=source.find(literal);pos!=std::string::npos;pos=source.find(literal,pos+1))
                if ((pos==0||!wordByte(source[pos-1]))&&(pos+literal.size()==source.size()||!wordByte(source[pos+literal.size()]))) {
                    argumentSpans.emplace_back(pos, pos + literal.size());
                    return true;
                }
            return false;
        };
        for (const auto& [key, argument] : action->arguments()) {
            const auto schema=std::find_if(description->definition->parameters.begin(),description->definition->parameters.end(),[&](const auto& p){return p.id==key;});
            bool present = std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T,std::string>) {
                    if (schema->type=="enum") {
                        int count=0;for(const auto& choice:schema->values)if(literalPresent(choice))++count;
                        if(count!=1)return false;
                    }
                    if(!literalPresent(value))return false;
                    for(auto pos=source.find(value);pos!=std::string::npos;pos=source.find(value,pos+1))
                        for(auto& number:numbers)if(number.begin>=pos&&number.end<=pos+value.size())number.consumed=true;
                    return true;
                } else if constexpr (std::is_same_v<T,bool>) {
                    return literalPresent(value?"true":"false")&&!literalPresent(value?"false":"true");
                } else {
                    std::size_t found=0;
                    for(auto& number:numbers) {
                        if(number.consumed)continue;
                        bool matches=false;
                        if constexpr (std::is_same_v<T,std::int64_t>) {
                            std::int64_t parsed=0;const auto result=std::from_chars(number.text.data(),number.text.data()+number.text.size(),parsed);
                            matches=result.ec==std::errc{}&&result.ptr==number.text.data()+number.text.size()&&parsed==value;
                        } else {
                            errno=0;char* end=nullptr;const auto parsed=std::strtod(number.text.c_str(),&end);
                            matches=errno!=ERANGE&&std::isfinite(parsed)&&end==number.text.c_str()+number.text.size()&&parsed==value;
                        }
                        if(matches){number.consumed=true;number.numericArgument=true;++found;}
                    }
                    return found==1;
                }
            }, argument.data());
            if (!present) return actions::Unsupported{};
        }
        if(std::any_of(numbers.begin(),numbers.end(),[](const auto& n){return !n.consumed;})) return actions::Unsupported{};
        const bool percent = std::any_of(description->definition->parameters.begin(), description->definition->parameters.end(),
            [](const auto& parameter) { return parameter.unit == "percent"; });
        for (const auto& number : numbers) {
            const auto end = percent && number.end < source.size() && source[number.end] == '%' ? number.end + 1 : number.end;
            argumentSpans.emplace_back(number.begin, end);
            if (number.numericArgument) numericArgumentSpans.emplace_back(number.begin, end);
        }
        return requestCovered(original, *description, grounding, argumentSpans, numericArgumentSpans) ? candidate : actions::CandidateResult{actions::Unsupported{}};
    }
    if (!description->brightness) return actions::Unsupported{};
    // Relative/unspecified brightness is unresolved, even if scoped inference
    // emits a structurally valid action. Never let model confidence supply a value.
    if (!state.specified) return actions::Unsupported{};
    const auto brightness = *description->brightness;
    if (brightness < state.minimum || brightness > state.maximum) return actions::Unsupported{};
    // Ground the target through snapshot aliases. Multiple different room
    // identities are ambiguous even if one happens to match the candidate.
    if (grounding.locations.size() != 1 || !description->target->roomId ||
        grounding.locations.front() != *description->target->roomId ||
        (!grounding.devices.empty() && grounding.devices.front() != action->targetId())) return actions::Unsupported{};
    // Conservative stem detection also covers inflections and mixed case.
    if (brightness == 0 && detail::lowercase(original).find("включ") != std::string::npos)
        return actions::Unsupported{};
    return requestCovered(original, *description, grounding) ? candidate : actions::CandidateResult{actions::Unsupported{}};
}
}
