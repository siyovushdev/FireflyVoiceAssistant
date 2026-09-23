#include "TypedRuntimeChecks.h"
using namespace runtime_test;
int main(int argc, char** argv) { try {
    if(argc!=2)throw std::runtime_error("root required");
    Runtime runtime(config::ConfigLoader::fromFile(std::string(argv[1])+"/config/current-m4.json"));
    for (const auto& location : runtime.domain.parserLocations()) {
        for (int value=0;value<=100;++value) {
            const auto n=std::to_string(value);
            for (const auto& input : {location.phrase+" свет "+n,"свет "+location.phrase+" "+n,
                    "установи свет "+location.phrase+" на "+n, location.phrase+" поставь свет на "+n+" процентов",
                    "сделай свет "+location.phrase+" на "+n+"%", "установи яркость света "+location.phrase+" на "+n}) {
                for (const auto& text : {input," \t"+input+"!?  "}) {
                    const auto parsed=runtime.parser.parse(text);
                    check(action(parsed)==light(location.targetId,value),"parser constructs exact stable-ID candidate directly");
                    const auto old=reference::FrozenParser::parse(text);
                    check(old && *old==runtime.codec.render(parsed),"frozen grammar/compact output equivalence");
                    const auto result=runtime.engine.process(text);
                    check(result.action==parsed && result.path==CommandPath::FastPath,"shared typed fast path");
                    check(runtime.llm.calls==0,"deterministic route bypasses model");
                }
            }
        }
    }
    for(const auto& [mention,id]:runtime.domain.snapshot()->mentionIndex()) {
        for(const auto& target:runtime.domain.snapshot()->configuration().devices) {
            if(!target.roomId)continue;
            const auto result=SafetyGate::apply(mention+" свет 40",light(target.id,40),runtime.domain);
            check(unsupported(result)==(*target.roomId!=id),"all aliases ground only their configured target");
        }
    }
    for(const auto* input:{"на кухне и в спальне свет 40","на кухне свет -1","на кухне свет 101",
            "свет 40","в детской свет 40","На  кухне свет 40","на кухнейке свет 40","не включи свет на кухне"})
        check(unsupported(runtime.parser.parse(input)),"unsupported/ambiguous grammar remains unresolved");
    check(unsupported(SafetyGate::apply("на кухне и в спальне свет 40",light("device_kitchen_light",40),runtime.domain)),"ambiguous known target evidence rejects injected candidate");
    auto changed=runtime.domain.snapshot()->configuration();
    auto& living=changed.rooms.at(runtime.domain.snapshot()->roomIndex().at("room_living"));
    auto& bedroom=changed.rooms.at(runtime.domain.snapshot()->roomIndex().at("room_bedroom"));
    living.locations.erase(std::find(living.locations.begin(),living.locations.end(),"в зале"));
    bedroom.locations.push_back("в зале");
    for(const std::string alias:{"зал","зале","зала","залу","залом"}) {
        living.mentions.erase(std::find(living.mentions.begin(),living.mentions.end(),alias));bedroom.mentions.push_back(alias);
    }
    Runtime remapped(config::ConfigValidator::validate(changed));
    const auto result=remapped.engine.process("в зале свет 40");
    check(action(result.action)==light("device_bedroom_light",40),"configured alias remapping drives parser AND safety; no legacy veto/fallback");
    check(remapped.codec.render(result.action)=="L|B|40","compatibility boundary reflects semantic target");
    check(reference::FrozenParser::parse("в зале свет 40")=="L|G|40","independent oracle visibly differs, never chooses result");
    changed=runtime.domain.snapshot()->configuration();auto duplicate=changed.devices.front();duplicate.id="test_extra_light";
    if(duplicate.kind!=config::DeviceKind::Light)duplicate=changed.devices.at(runtime.domain.snapshot()->deviceIndex().at("device_kitchen_light"));
    duplicate.id="test_extra_light";changed.devices.push_back(duplicate);
    bool failed=false;try { Runtime ambiguous(config::ConfigValidator::validate(changed)); } catch(const config::ConfigError& error) {failed=error.code=="ambiguous_target";}
    check(failed,"two light targets for one grammar location cannot select first target");
    return finish("ParserRoomCutover");
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
