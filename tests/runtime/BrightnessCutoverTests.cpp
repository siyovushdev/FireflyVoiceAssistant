#include "TypedRuntimeChecks.h"
using namespace runtime_test;
int main(int argc,char** argv) {try {
    if(argc!=2)throw std::runtime_error("root required");
    const auto sample=config::ConfigLoader::fromFile(std::string(argv[1])+"/config/current-m4.json");
    for(const auto& [minimum,maximum]:{std::pair{0,100},std::pair{10,90},std::pair{0,0},std::pair{100,100},std::pair{50,50}}) {
        auto cfg=sample->configuration();for(auto& a:cfg.actions)if(!a.parameters.empty()){a.parameters[0].minimum=minimum;a.parameters[0].maximum=maximum;}
        Runtime runtime(config::ConfigValidator::validate(cfg));
        for(const auto& location:runtime.domain.parserLocations()) {
            for(int value=-1;value<=101;++value) {
                const auto candidate=light(location.targetId,value);const bool supported=value>=minimum&&value<=maximum;
                check(unsupported(runtime.domain.validate(candidate))!=supported,"configured range is semantic authority");
                if(value<0||value>100)continue;
                for(const auto& text:{location.phrase+" свет "+std::to_string(value),"Пожалуйста, "+location.phrase+" свет "+std::to_string(value)}) {
                    runtime.llm.raw=runtime.codec.render(candidate);
                    const auto result=runtime.engine.process(text);
                    check(unsupported(result.action)!=supported,"both routes enforce identical configured bounds");
                    if(supported)check(action(result.action)==candidate,"accepted value/identity unchanged without clamp");
                }
            }
            for(const auto value:{std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()})
                check(unsupported(runtime.domain.validate(light(location.targetId,value))),"int64 extremes reject without narrowing");
            for(const auto value:{0,50}) {
                runtime.llm.raw=runtime.codec.render(light(location.targetId,value));
                check(unsupported(runtime.engine.process(location.phrase+" выключи свет на 50 процентов").action),"contradiction rejects observed and guessed-zero candidates");
            }
        }
        check(unsupported(runtime.domain.validate(GenericAction("action_set_brightness","missing",{{"brightness",IntegerArgument(std::int64_t{40})}}))),"unknown target fails");
        check(unsupported(runtime.domain.validate(GenericAction("missing","device_kitchen_light",{}))),"unknown action fails");
        check(unsupported(runtime.domain.validate(GenericAction("action_set_brightness","device_kitchen_light",{}))),"missing brightness fails");
        check(unsupported(runtime.domain.validate(GenericAction("action_set_brightness","device_kitchen_light",{{"extra",IntegerArgument(std::int64_t{40})}}))),"wrong argument identity fails");
    }
    Runtime current(sample);
    for(const auto* raw:{"L|K|-1","L|K|101","L|K|abc","L|K|","L|K|50|extra"}) {
        current.llm.raw=raw;const auto result=current.engine.process("Пожалуйста на кухне свет 50");
        check(unsupported(result.action)&&result.invalidModelOutput,"malformed model brightness is not semantic action");
    }
    auto cfg=sample->configuration();for(auto& d:cfg.devices)d.enabled=false;for(auto& a:cfg.actions)a.enabled=false;
    Runtime disabled(config::ConfigValidator::validate(cfg));
    check(action(disabled.engine.process("на кухне свет 40").action)==light("device_kitchen_light",40),"enablement policy deliberately not activated by representation migration");
    return finish("BrightnessCutover");
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
