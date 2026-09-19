#include "firefly/actions/GenericAction.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace firefly::actions;
int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
static_assert(!std::is_default_constructible_v<GenericAction>);
static_assert(!std::is_default_constructible_v<IntegerArgument>);
static_assert(std::is_copy_constructible_v<GenericAction> && std::is_copy_assignable_v<GenericAction>);
static_assert(std::is_move_constructible_v<GenericAction> && std::is_move_assignable_v<GenericAction>);
static_assert(std::is_copy_constructible_v<IntegerArgument>);
static_assert(std::is_constructible_v<IntegerArgument, std::int64_t>);
static_assert(!std::is_convertible_v<std::int64_t, IntegerArgument>);
static_assert(!std::is_constructible_v<IntegerArgument, bool>);
static_assert(!std::is_constructible_v<IntegerArgument, double>);
static_assert(!std::is_constructible_v<IntegerArgument, float>);
static_assert(!std::is_constructible_v<IntegerArgument, std::uint64_t>);
static_assert(!std::is_constructible_v<IntegerArgument, std::string>);
static_assert(!std::is_constructible_v<IntegerArgument, const char*>);
static_assert(!std::is_constructible_v<IntegerArgument, std::nullptr_t>);
static_assert(std::is_same_v<decltype(std::declval<GenericAction&>().arguments()), const GenericAction::Arguments&>);
static_assert(std::is_same_v<decltype(std::declval<GenericAction&>().targetId()), const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<GenericAction&>().actionId()), const std::string&>);
}

int main() {
    try {
        GenericAction::Arguments arguments{{"brightness", IntegerArgument(std::int64_t{40})}};
        std::string actionId = "action_set_brightness", targetId = "device_kitchen_light";
        const GenericAction light(actionId, targetId, arguments);
        actionId = "changed";
        targetId = "changed";
        arguments.at("brightness") = IntegerArgument(std::int64_t{90});
        check(light.actionId() == "action_set_brightness", "owns action ID");
        check(light.targetId() == "device_kitchen_light", "owns target ID, not a room code");
        check(light.arguments().at("brightness").value() == 40, "owns integer arguments without mutable aliases");
        const auto copy = light;
        check(copy == light, "copy equality");
        auto movedSource = copy;
        const auto moved = std::move(movedSource);
        check(moved == light, "move equality");
        GenericAction assigned("unused", "unused", {});
        assigned = light;
        check(assigned == light, "value assignment does not mutate the source");
        check(light != GenericAction("other_action", light.targetId(), light.arguments()), "action ID participates in equality");
        check(light != GenericAction(light.actionId(), "other_target", light.arguments()), "target ID participates in equality");
        check(light != GenericAction(light.actionId(), light.targetId(), {}), "missing argument differs");
        check(light != GenericAction(light.actionId(), light.targetId(), arguments), "argument value differs");
        check(light != GenericAction(light.actionId(), light.targetId(), {{"other", IntegerArgument(std::int64_t{40})}}),
              "argument identity differs");

        const GenericAction door("action_open_entrance", "device_entrance_door", {});
        check(door.arguments().empty(), "door has no invented numeric value");
        check(door != light, "door and light differ");
        CandidateResult absent;
        check(std::holds_alternative<Unsupported>(absent), "default result is Unsupported");
        CandidateResult candidate = light;
        check(std::holds_alternative<GenericAction>(candidate), "explicit action alternative");
        check(candidate != absent, "Unsupported is not an Action");
        check(candidate == CandidateResult{copy}, "result semantic equality");
        candidate = Unsupported{};
        check(candidate == absent, "rejecting removes the action");

        // Generic values do not claim schema or safety validation. Ordered keys
        // give stable equality even for a candidate that an adapter must reject.
        const GenericAction first("a", "t", {{"z", IntegerArgument(std::int64_t{2})},
                                               {"a", IntegerArgument(std::int64_t{1})}});
        const GenericAction second("a", "t", {{"a", IntegerArgument(std::int64_t{1})},
                                                {"z", IntegerArgument(std::int64_t{2})}});
        check(first == second, "argument insertion order is not semantic identity");
        check(first.arguments().begin()->first == "a", "deterministic argument order");
        check(IntegerArgument(std::numeric_limits<std::int64_t>::min()).value() == std::numeric_limits<std::int64_t>::min(),
              "signed minimum stored without narrowing; adapter may reject it");
        check(IntegerArgument(std::numeric_limits<std::int64_t>::max()).value() == std::numeric_limits<std::int64_t>::max(),
              "signed maximum stored without narrowing; adapter may reject it");
        std::cout << "GenericAction: " << checks << " checks passed; integer-only type constraints compiled\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GenericAction failure: " << error.what() << '\n';
        return 1;
    }
}
