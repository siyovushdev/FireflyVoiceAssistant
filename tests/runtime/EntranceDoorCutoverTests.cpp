#include "TypedRuntimeChecks.h"
using namespace runtime_test;
int main(int argc,char** argv) {try {
    if(argc!=2)throw std::runtime_error("root required");
    const auto sample=config::ConfigLoader::fromFile(std::string(argv[1])+"/config/current-m4.json");
        std::vector<std::pair<std::string, std::string>> matrix{
            {"дверь", "U"}, {"открой дверь", "U"},
            {"открой входную дверь", "D"}, {"открыть дверь на входе", "D"},
            {"ОТКРОЙ дверь НА\tВХОДЕ!", "D"}, {"  ОТКРОЙ ВХОДНУЮ ДВЕРЬ!  ", "D"},
            {"двер", "U"}, {"входная дверь", "U"}, {"открой входную", "U"},
            {"закрой входную дверь", "U"}, {"Разблокируй входную дверь", "U"},
            {"Открой дверь в спальню", "U"}, {"открой дверь в ванную", "U"},
            {"открой входную тверь", "U"}, {"аткрой входную дверь", "U"},
            {"открытая входная дверь", "U"}, {"открой вход", "U"},
            {"дверь на входе", "U"}, {"открой на входе", "U"},
            {"открой тверь на входе", "U"}, {"открой дверь на входеже", "U"},
            {"открой дверь страна входе", "U"}, {"открой дверь на другом входе", "U"},
            {"открытвер на входе", "U"}};
        for (const std::string intent : {"открой", "откройте", "открыть", "открывай", "открывайте"}) {
            matrix.emplace_back(intent + " входную дверь", "D");
            matrix.emplace_back(intent + " дверь на входе", "D");
        }
        // Use the original frozen labels, including related negative DEV cases.
        std::ifstream dev(std::string(argv[1]) + "/tests/data/dev.tsv");
        check(bool(dev), "frozen DEV readable");
        std::string line;
        int devDoors = 0;
        while (std::getline(dev, line)) {
            std::istringstream fields(line);
            std::string id, category, expected, input;
            std::getline(fields, id, '\t'); std::getline(fields, category, '\t');
            std::getline(fields, expected, '\t'); std::getline(fields, input);
            if (input.find("двер") != std::string::npos) {
                matrix.emplace_back(input, expected);
                ++devDoors;
            }
        }
        check(devDoors == 6, "all six existing door-related DEV cases included");
        std::cout << "Door safety matrix: " << matrix.size() << " rows, " << devDoors << " frozen DEV rows\n";


    CandidateResult retained;
    for(const std::string name:{"Входная дверь","Главный вход — тест","дверь"}) {
        auto cfg=sample->configuration();cfg.devices.at(sample->deviceIndex().at(cfg.compatibility.doorTargetId)).name=name;
        std::reverse(cfg.devices.begin(),cfg.devices.end());
        Runtime runtime(config::ConfigValidator::validate(cfg));
        for(const auto& [input,expected]:matrix) {
            check(unsupported(runtime.parser.parse(input)),"door remains fallback");
            for(const std::string raw:{"D"," \nD\t","U","","D|device_entrance_door","D|unknown_door"}) {
                runtime.llm.raw=raw;const auto before=runtime.llm.calls;const auto result=runtime.engine.process(input);
                const bool valid=raw=="D"||raw==" \nD\t";
                check(runtime.codec.render(result.action)==(valid?expected:"U"),"unchanged door safety compact matrix");
                check(runtime.llm.calls==before+1&&runtime.llm.original==input,"single inference with original input");
                if(valid&&expected=="D") {
                    const auto& value=action(result.action);
                    check(value.actionId()=="action_open_entrance"&&value.targetId()=="device_entrance_door"&&value.arguments().empty(),"canonical door carries configured stable IDs, no empty legacy Action");
                    check(runtime.domain.device(value.targetId())->name==name,"display metadata remains snapshot-derived");
                    retained=result.action;
                } else check(unsupported(result.action),"rejected result contains no executable GenericAction");
            }
        }
        check(unsupported(runtime.domain.validate(GenericAction("action_open_entrance","missing",{}))),"unknown door fails without fallback");
        check(unsupported(runtime.domain.validate(GenericAction("action_open_entrance","device_kitchen_light",{}))),"wrong target kind fails");
        check(unsupported(runtime.domain.validate(GenericAction("action_open_entrance","device_entrance_door",{{"brightness",IntegerArgument(std::int64_t{50})}}))),"door cannot carry numeric argument");
    }
    check(action(retained).targetId()=="device_entrance_door","semantic target survives runtime lifetime");
    auto cfg=sample->configuration();auto extra=cfg.devices.at(sample->deviceIndex().at(cfg.compatibility.doorTargetId));extra.id="extra_entrance";cfg.devices.push_back(extra);
    Runtime extraRuntime(config::ConfigValidator::validate(cfg));
    check(unsupported(extraRuntime.domain.validate(GenericAction("action_open_entrance",extra.id,{}))),"unselected configured entrance is not a substitute");
    cfg.compatibility.doorTargetId="missing";invalidConfig(cfg);
    return finish("EntranceDoorCutover");
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
