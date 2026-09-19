#include "firefly/runtime/RuntimeConfig.h"
#include "firefly/voice/VoiceProcessor.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/RkllmEngine.h"
#include "firefly/llm/LlmContext.h"
#include "firefly/actions/CompactV1Adapter.h"
#include "c-api.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
using namespace firefly;
using namespace firefly::voice;
int main(int argc, char** argv) {
    try {
        if (argc != 6) throw std::runtime_error("Usage: generic_speech_replay_tests STT_DIR VAD WORK MODEL CONFIG");
        const runtime::RuntimeConfig owner(argv[5]);
        const DeterministicParser parser(owner.domain());
        const actions::CompactV1Adapter output(owner.domain().snapshot());
        SherpaVad vad(argv[2]);
        SherpaRecognizer recognizer(argv[1]);
        RkllmConfig configuration;
        configuration.modelPath = argv[4]; configuration.systemPrompt = llm::generateContext(owner.domain()).systemPrompt();
        RkllmEngine llm(std::move(configuration));
        CommandEngine engine(llm, parser, owner.domain());
        VoiceProcessor processor(recognizer, engine);
        const char* names[] = {"01_door.wav", "02_open_door.wav", "03_open_entrance_door.wav", "04_light_on.wav",
            "05_light_off.wav", "06_kitchen_40.wav", "07_bedroom_half.wav", "08_corridor_off.wav", "silence-2s.wav", "silence-5s.wav"};
        const char* texts[] = {"дверь", "открой дверь", "открой входную дверь", "включи свет", "выключи свет",
            "на кухне поставь свет на сорок процентов", "в спальне свет наполовину", "в коридоре выключи свет"};
        const char* expected[] = {"U", "U", "D", "U", "U", "L|K|40", "L|B|50", "L|C|0"};
        int calls = 0;
        for (int i = 0; i < 10; ++i) {
            vad.reset(); VoiceSegmenter segments(vad);
            const auto file = std::string(argv[3]) + (i < 8 ? "/eval/" : "/evaluation/") + names[i];
            std::unique_ptr<const SherpaOnnxWave, decltype(&SherpaOnnxFreeWave)> wave(SherpaOnnxReadWave(file.c_str()), SherpaOnnxFreeWave);
            if (!wave || wave->sample_rate != SampleRate) throw std::runtime_error("Invalid replay WAV");
            int utterances = 0;
            for (int offset = 0; offset < wave->num_samples; offset += FrameSamples) {
                for (const auto& utterance : segments.accept(wave->samples + offset, std::min(FrameSamples, wave->num_samples - offset))) {
                    if (i >= 8) throw std::runtime_error("Silence produced a command");
                    const auto result = processor.process(utterance);
                    ++utterances; ++calls;
                    const auto final = output.render(result.action);
                    const auto semantic = std::get<actions::CandidateResult>(output.decode(expected[i]));
                    if (!result.error.empty() || !result.recognition.success || result.recognition.text != texts[i] || result.action != semantic)
                        throw std::runtime_error("Typed voice replay differs: " + std::string(names[i]) + " final=" + final);
                    std::cout << names[i] << " text=" << result.recognition.text << " final=" << final << " route=" << result.route << std::endl;
                }
            }
            if (utterances != (i < 8 ? 1 : 0)) throw std::runtime_error("Replay utterance count changed");
            if (i >= 8) std::cout << names[i] << " utterances=0 commands=0" << std::endl;
        }
        std::cout << "PASS: 8/8 typed voice actions and exact transcripts; 2/2 silences; calls=" << calls << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
