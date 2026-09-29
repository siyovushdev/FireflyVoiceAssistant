#include "firefly/voice/WakeVoiceLoop.h"
#include "firefly/voice/SherpaSpeech.h"
#include "c-api.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>
using namespace firefly::voice;
using Json = nlohmann::json;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
long rssKiB() {
    std::ifstream input("/proc/self/smaps_rollup"); std::string line;
    while(std::getline(input,line))if(line.rfind("Rss:",0)==0)return std::stol(line.substr(4));
    throw std::runtime_error("Cannot read RSS");
}
struct Wake : WakeDetector {
    int calls = 0, trigger;
    explicit Wake(int frame) : trigger(frame) {}
    bool accept(const float*, int) override { return ++calls == trigger; }
    void reset() override {}
};
struct Ack : WakeAcknowledgement {
    int starts = 0;
    void start() override { ++starts; }
    AcknowledgementStatus poll() override { return AcknowledgementStatus::Complete; }
    void cancel() noexcept override {}
};
struct Source : AudioSource {
    std::function<void(float*,int)> next;
    int read(float* data, int n) override { next(data,n); return n; }
};
int main(int argc, char** argv) { try {
    if (argc != 5) throw std::runtime_error("Usage: seamless_speech_replay STT VAD DEV_EVAL REPORT");
    SherpaVad vad(argv[2]); SherpaRecognizer recognizer(argv[1]);
    Json report = {{"kind","recorded DEV command PCM; fake KWS admission; real VAD/STT; no recorded Amina prefix available"},{"cases",Json::array()}};
    for (const auto* name : {"06_kitchen_40.wav","04_light_on.wav","05_light_off.wav"}) {
        const auto path = std::string(argv[3]) + "/" + name;
        std::unique_ptr<const SherpaOnnxWave,decltype(&SherpaOnnxFreeWave)> wav(SherpaOnnxReadWave(path.c_str()),SherpaOnnxFreeWave);
        require(wav && wav->sample_rate==SampleRate,"Invalid DEV WAV");
        VoiceSegmenter trim(vad); trim.startSession(); std::vector<Utterance> original;
        for (int i=0;i<wav->num_samples;i+=FrameSamples) {
            auto ready=trim.accept(wav->samples+i,std::min(FrameSamples,wav->num_samples-i));
            original.insert(original.end(),ready.begin(),ready.end());
        }
        require(original.size()==1 && original[0].error.empty(),"Expected one recorded utterance");
        auto clip=original[0].audio;
        const auto expected=recognizer.recognize(clip);
        // Exercise an almost-full production ring, not just startup's short history.
        constexpr int WarmPaddingMs = 2400;
        clip.insert(clip.begin(),WarmPaddingMs*16,0);
        require(expected.success && !expected.text.empty(),"DEV STT failed");
        // Trim supplies 300 ms pre-roll; KWS is delayed another 256 ms into speech.
        const int trigger=(WarmPaddingMs+300+256+31)/32;
        auto probe = std::vector<float>(clip.begin(),clip.begin()+trigger*512+4096);
        VoiceSegmenter decisionProbe(vad);
        require(decisionProbe.continuesAfterWake(probe,trigger*512),"Recorded continuation misclassified");
        std::fill(probe.begin()+trigger*512,probe.end(),0);
        require(!decisionProbe.continuesAfterWake(probe,trigger*512),"Silero hangover misclassified as continuation");
        Wake wake(trigger); Ack ack; Source source; VoiceSegmenter segmenter(vad);
        auto time=std::chrono::steady_clock::time_point{};
        auto nextRead=std::chrono::steady_clock::now(), wakeWall=nextRead;
        double decisionWallMs=0, maxReadGapMs=0;
        auto previousRead=nextRead;
        const auto baselineRss=rssKiB(); auto peakCaptureRss=baselineRss;
        int reads=0, waits=0, emitted=0; size_t offset=0; bool stop=false; std::string phase;
        Json events=Json::array(); VoiceResult result;
        source.next=[&](float* pcm,int n) {
            nextRead+=std::chrono::milliseconds(32);std::this_thread::sleep_until(nextRead);
            const auto readTime=std::chrono::steady_clock::now();
            maxReadGapMs=std::max(maxReadGapMs,std::chrono::duration<double,std::milli>(readTime-previousRead).count());
            previousRead=readTime;
            if(phase!="PROCESSING")peakCaptureRss=std::max(peakCaptureRss,rssKiB());
            time+=std::chrono::milliseconds(32);std::fill(pcm,pcm+n,0);
            require(++reads<30000,"Replay did not terminate");
            if (waits>=2) stop=true;
            auto count=std::min<size_t>(n,clip.size()-offset);
            std::copy_n(clip.data()+offset,count,pcm);offset+=count;
            if (phase=="PROCESSING") std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        runWakeVoiceLoop(source,wake,segmenter,{},[&](const Utterance& u) {
            require(u.inlineWake,"Real VAD failed to confirm continuation");
            // Exact waveform evidence: retain the first 256 ms of speech that
            // occurred before fake KWS detection, independent of STT accuracy.
            const auto first=clip.begin()+(WarmPaddingMs+300)*16;
            require(std::search(u.audio.begin(),u.audio.end(),first,first+256*16)!=u.audio.end(),"Pre-wake command onset missing");
            VoiceProcessor processor(recognizer,[](std::string_view) -> firefly::CommandResult {
                throw std::runtime_error("Missing Amina prefix must not reach CommandEngine");
            });
            return processor.process(u);
        },[&](const VoiceResult& value) { result=value;++emitted; },
        [&](const std::string& value) { phase=value;
            if(value=="WAKE_DETECTED")wakeWall=std::chrono::steady_clock::now();
            if(value=="CONTINUATION_INLINE" || value=="CONTINUATION_SEPARATED")
                decisionWallMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wakeWall).count();
            events.push_back({{"state",value},{"ms",std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count()}});if(value=="WAITING_WAKE")++waits; },
        [&] { return stop; },[&] { return time; },&ack);
        require(emitted==1 && ack.starts==0,"Inline playback/session count incorrect");
        require(result.recognition.text==expected.text,"Real STT lost command beginning");
        require(!result.wakePrefixRemoved && result.route=="NOT_CALLED" && !result.error.empty(),"Unconfirmed wake transcript was executed");
        report["cases"].push_back({{"file",name},{"expected",expected.text},{"observed",result.recognition.text},
            {"pre_detection_speech_ms",256},{"wake_ring_history_ms",trigger*32},{"paced_detection_to_decision_wall_ms",decisionWallMs},{"max_capture_read_gap_ms",maxReadGapMs},{"warm_models_to_capture_added_rss_kib",peakCaptureRss-baselineRss},{"beeps",ack.starts},{"stt_ms",result.recognition.latencyMs},{"events",events},{"prefix_rejected",true}});
        std::cout<<name<<": "<<result.recognition.text<<"; onset preserved, missing prefix safely rejected\n"<<std::flush;
    }
    std::ofstream(argv[4])<<report.dump(2)<<'\n';
} catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
