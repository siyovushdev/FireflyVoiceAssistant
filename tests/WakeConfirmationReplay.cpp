// Explicit saved-PCM tool, not an automatically registered hardware test.
// Uses real production STT + confirmation; never opens ALSA or command runtime.
#include "firefly/voice/WakeConfirmation.h"
#include "firefly/voice/SherpaSpeech.h"
#include "c-api.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using Json=nlohmann::json;
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Usage: wake_confirmation_replay STT_DIR INPUT_JSON REPORT_JSON");
    Json inputs;std::ifstream(argv[2])>>inputs;
    if(!inputs.is_array() || inputs.size()>10000)throw std::runtime_error("Invalid bounded replay manifest");
    firefly::voice::SherpaRecognizer recognizer(argv[1]);
    firefly::voice::SttWakeConfirmation verifier(recognizer);
    Json out=Json::array();
    for(auto row:inputs){
        const std::string path=row.at("path");
        std::unique_ptr<const SherpaOnnxWave,decltype(&SherpaOnnxFreeWave)> wave(SherpaOnnxReadWave(path.c_str()),SherpaOnnxFreeWave);
        if(!wave || wave->sample_rate!=16000)throw std::runtime_error("Invalid replay WAV");
        const int start=row.at("start_sample"),end=row.at("end_sample");
        if(start<0 || end<=start || end>wave->num_samples || end-start>52096)throw std::runtime_error("Invalid bounded replay window");
        const auto result=verifier.confirm({wave->samples+start,wave->samples+end});
        Json attempts=Json::array();
        for(const auto& a:result.attempts)attempts.push_back({{"text",a.text},{"success",a.success},{"stt_ms",a.latencyMs}});
        row["accepted"]=result.accepted;row["reason"]=result.reason;row["processing_ms"]=result.processingMs;row["attempts"]=attempts;
        out.push_back(row);
    }
    std::ofstream output(argv[3]);
    output<<Json({{"scope","saved PCM, real C++ SherpaRecognizer/SttWakeConfirmation; no microphone/commands"},{"rows",out}}).dump(2)<<'\n';
    output.close();if(!output)throw std::runtime_error("Cannot write replay report");
    std::cout<<out.size()<<" real C++ STT confirmations completed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
