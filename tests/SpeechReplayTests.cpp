#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/VoiceSegmenter.h"
#include "c-api.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace firefly::voice;
int main(int argc, char** argv) { try {
    if(argc != 4) throw std::runtime_error("Usage: speech_replay_tests STT_DIR VAD_MODEL STT_WORK_DIR");
    SherpaVad vad(argv[2]); SherpaRecognizer stt(argv[1]);
    const char* names[]={"01_door.wav","02_open_door.wav","03_open_entrance_door.wav","04_light_on.wav","05_light_off.wav","06_kitchen_40.wav","07_bedroom_half.wav","08_corridor_off.wav","silence-2s.wav","silence-5s.wav"};
    const char* texts[]={"дверь","открой дверь","открой входную дверь","включи свет","выключи свет","на кухне поставь свет на сорок процентов","в спальне свет наполовину","в коридоре выключи свет"};
    int total=0;
    for(int i=0;i<10;++i){
        vad.reset(); VoiceSegmenter segments(vad);
        std::string file=std::string(argv[3])+(i<8?"/eval/":"/evaluation/")+names[i];
        std::unique_ptr<const SherpaOnnxWave,decltype(&SherpaOnnxFreeWave)> w(SherpaOnnxReadWave(file.c_str()),SherpaOnnxFreeWave);
        if(!w||w->sample_rate!=SampleRate)throw std::runtime_error("Invalid test WAV");
        int calls=0;
        for(int offset=0;offset<w->num_samples;offset+=FrameSamples){
            auto completed=segments.accept(w->samples+offset,std::min(FrameSamples,w->num_samples-offset));
            for(auto& u:completed){
                if(i>=8)throw std::runtime_error("Silence invoked STT");
                auto r=stt.recognize(u.audio);++calls;++total;
                std::cout<<names[i]<<" duration="<<double(u.audio.size())/SampleRate<<" text="<<r.text<<" stt_ms="<<r.latencyMs<<std::endl;
                if(!u.error.empty()||!r.success||r.text!=texts[i])throw std::runtime_error("Replay differs from selected 300 ms DEV baseline");
            }
        }
        if(calls!=(i<8?1:0))throw std::runtime_error("Incorrect utterance count");
    }
    // Independent request failure must not poison/replay the next inference.
    if(stt.recognize({}).success)throw std::runtime_error("Empty input accepted");
    std::cout<<"PASS: eight streamed speech WAVs, two silences, one long-lived STT instance; STT calls="<<total<<'\n';
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
