#include "firefly/voice/WakePcmRing.h"
#include "firefly/voice/VoiceSegmenter.h"
#include "firefly/voice/SherpaSpeech.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
using namespace firefly::voice;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
long rssKiB() {
    std::ifstream input("/proc/self/smaps_rollup"); std::string line;
    while (std::getline(input,line)) if (line.rfind("Rss:",0)==0) return std::stol(line.substr(4));
    throw std::runtime_error("Cannot read RSS");
}
int main(int argc,char** argv) { try {
    if(argc!=3) throw std::runtime_error("Usage: seamless_wake_benchmark VAD REPORT");
    std::array<float,FrameSamples> frame{};
    const auto before=rssKiB(); auto ring=std::make_unique<WakePcmRing>();
    ring->append(frame.data(),frame.size()); const auto after=rssKiB();
    constexpr int frames=600*SampleRate/FrameSamples;
    const auto cpu=std::clock(); const auto wall=Clock::now();
    for(int i=0;i<frames;++i) { frame[0]=float(i);ring->append(frame.data(),frame.size()); }
    const auto cpuMs=1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC;
    const auto wallMs=std::chrono::duration<double,std::milli>(Clock::now()-wall).count();
    auto snapshot=ring->snapshot(); snapshot.insert(snapshot.end(),4096,0);
    const auto peak=rssKiB();
    // Real, unchanged Silero: worst-sized bounded history plus endpoint probe,
    // followed by final real-PCM refeed. No STT, KWS or additional model instance.
    SherpaVad vad(argv[1]); VoiceSegmenter segmenter(vad);
    std::fill(snapshot.begin(),snapshot.end(),0);
    Json probe=Json::array();
    for(int i=0;i<30;++i) {
        const auto start=Clock::now(); const auto tick=std::clock();
        segmenter.continuesAfterWake(snapshot,WakePcmRing::Capacity);
        const auto decisionMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        for(size_t offset=0;offset<snapshot.size();offset+=FrameSamples)
            segmenter.accept(snapshot.data()+offset,std::min<size_t>(FrameSamples,snapshot.size()-offset));
        probe.push_back({{"decision_compute_ms",decisionMs},{"decision_plus_refeed_cpu_ms",1000.0*(std::clock()-tick)/CLOCKS_PER_SEC}});
        segmenter.startSession();
    }
    Json report={{"platform","Firefly ROC-RK3588S-PC"},{"ring_capacity_samples",WakePcmRing::Capacity},
        {"ring_pcm_bytes",WakePcmRing::Capacity*sizeof(float)},{"ring_added_rss_kib",after-before},
        {"ring_and_snapshot_added_rss_kib",peak-before},{"simulated_audio_seconds",frames*512.0/SampleRate},
        {"ring_cpu_ms",cpuMs},{"ring_wall_ms",wallMs},{"ring_one_core_percent",cpuMs/(frames*512.0/SampleRate*1000)*100},
        {"grace_audio_ms",256},{"probe_input","silence; full 3 s history + 256 ms grace; 1280 ms endpoint-only zeros"},
        {"probe_measurements",probe}};
    std::ofstream(argv[2])<<report.dump(2)<<'\n';std::cout<<report.dump(2)<<'\n';
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
