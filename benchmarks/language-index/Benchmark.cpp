#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/config/ConfigLoader.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <unistd.h>
using namespace firefly;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct Model:admin::ContextModel {
    int calls=0;
    llm::CommandProtocol commandProtocol()const override{return llm::CommandProtocol::StableCompact;}
    void useContext(const std::string&)override{}
    bool ready()const override{return true;}
    std::string infer(std::string_view)override{++calls;return "null";}
};
long rss(){std::ifstream in("/proc/self/statm");long pages=0,resident=0;in>>pages>>resident;return resident*sysconf(_SC_PAGESIZE);}
int main(int argc,char** argv){try{
    if(argc!=4)return 2;
    const auto catalog=config::ConfigLoader::fromFile(argv[1]);std::ifstream input(argv[2]);const auto corpus=Json::parse(input);
    Model model;admin::RuntimeGenerations runtime(model);runtime.apply(catalog);
    Json report{{"measurement","production routing with counting model; no NPU latency claim"},{"catalog_hash",catalog->contentHash()},{"rows",Json::array()}};
    for(const auto& item:corpus){const auto text=item.get<std::string>();const int before=model.calls;const auto result=runtime.process(text).command;
        Json row{{"input",text},{"qwen_calls",model.calls-before},{"path",static_cast<int>(result.path)},{"executable",std::holds_alternative<actions::GenericAction>(result.action)},{"safety_rejected",result.safetyRejected}};
#ifdef FIREFLY_LANGUAGE_INDEX
        row["index"]=result.indexStatus;row["route"]=result.languageRoute;
#endif
        report["rows"].push_back(row);
    }
    report["qwen_calls"]=model.calls;report["without_qwen"]=corpus.size()-model.calls;
#ifdef FIREFLY_LANGUAGE_INDEX
    std::vector<double> compile,match;std::vector<std::shared_ptr<const language::LanguageIndex>> retained;
    auto warm=language::LanguageCompiler::compile(catalog);
    const auto initial=rss();
    for(int i=0;i<256;++i){const auto start=Clock::now();retained.push_back(language::LanguageCompiler::compile(catalog));compile.push_back(std::chrono::duration<double,std::micro>(Clock::now()-start).count());}
    report["rss_bytes_per_index_256_retained"]=std::max(0L,rss()-initial)/256;
    report["estimated_owned_bytes"]=warm->approximateBytes();
    for(int repeat=0;repeat<100;++repeat)for(const auto& item:corpus){const auto text=item.get<std::string>();const auto start=Clock::now();(void)warm->match(text);match.push_back(std::chrono::duration<double,std::micro>(Clock::now()-start).count());}
    std::sort(compile.begin(),compile.end());std::sort(match.begin(),match.end());
    report["compile_median_us"]=compile[compile.size()/2];report["compile_p95_us"]=compile[compile.size()*95/100];
    report["match_median_us"]=match[match.size()/2];report["match_p95_us"]=match[match.size()*95/100];
#endif
    std::ofstream(argv[3])<<report.dump(2)<<'\n';std::cout<<report.dump(2)<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
