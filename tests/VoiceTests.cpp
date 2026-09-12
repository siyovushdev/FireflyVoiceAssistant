#include "firefly/voice/VoiceProcessor.h"
#include <iostream>
#include <stdexcept>
using namespace firefly;
using namespace firefly::voice;
void require(bool p, const char* message) { if (!p) throw std::runtime_error(message); }
struct Detector : SpeechDetector {
    Detection next; int resets = 0;
    Detection accept(const float*, int) override { auto r = next; next.completed.clear(); return r; }
    void reset() override { ++resets; next = {}; }
};
struct Recognizer : SpeechRecognizer {
    int calls = 0; bool fail = false, raises = false; std::string text;
    RecognitionResult recognize(const std::vector<float>&) override {
        ++calls; if (raises) throw std::runtime_error("backend failure");
        return {!fail, text, fail ? "failed" : "", 1};
    }
};
struct Llm : LlmEngine {
    int calls = 0; std::string original;
    std::string infer(std::string_view s) override { ++calls; original = s; return "U"; }
};
int main() { try {
    float frame[512]; for (int i = 0; i < 512; ++i) frame[i] = float(i) / 512;
    Detector d; VoiceSegmenter s(d);
    for (int i = 0; i < 1000; ++i) require(s.accept(frame, 512).empty(), "Silence generated utterance");
    require(s.bufferedSamples() <= 19000 * 16, "Unbounded ring");
    Detector padded; VoiceSegmenter ps(padded);
    for (int i = 0; i < 32; ++i) ps.accept(frame, 512); // 16384 samples
    padded.next.completed = {{8000, 16000}};
    require(ps.accept(frame, 512).empty(), "Did not wait for post-roll");
    std::vector<Utterance> output;
    for (int i = 0; i < 8; ++i) {
        auto ready = ps.accept(frame, 512); output.insert(output.end(), ready.begin(), ready.end());
    }
    require(output.size() == 1 && output[0].audio.size() == 17600, "Incorrect pre/post-roll");
    require(output[0].audio.front() == frame[3200 % 512] && output[0].audio.back() == frame[20799 % 512], "PCM samples changed");
    Detector beginning; VoiceSegmenter bs(beginning, {300, 0, 15000});
    beginning.next.completed = {{100, 400}};
    auto first = bs.accept(frame, 512);
    require(first.size() == 1 && first[0].audio.size() == 400, "Start clamp failed");
    Detector longSpeech; VoiceSegmenter ls(longSpeech, {300,300,1000});
    longSpeech.next.active = true; int rejects = 0;
    for (int i = 0; i < 200; ++i) for (auto& u : ls.accept(frame,512)) { require(!u.error.empty() && u.audio.empty(), "Truncated oversized speech"); ++rejects; }
    require(rejects == 1, "Long speech repeated or missing rejection");
    longSpeech.next.active = false;
    // Simulate forced VAD split, then continued speech: tail must not pass.
    longSpeech.next.completed = {{0, 100000}}; require(ls.accept(frame,512).empty(), "Oversize tail accepted");
    longSpeech.next.active = true;
    for (int i=0;i<20;++i) require(ls.accept(frame,512).empty(), "Continuation accepted");
    longSpeech.next.active = false;
    for (int i=0;i<33;++i) ls.accept(frame,512);
    require(!ls.active(), "Did not recover after quiet");
    ls.discontinuity(); require(ls.active(), "Discontinuity did not quarantine");
    for (int i=0;i<33;++i) require(ls.accept(frame,512).empty(), "Gap generated utterance");
    require(!ls.active(), "Gap recovery failed");
    bool bad=false; try { VoiceSegmenter invalid(d, {1001,300,15000}); } catch (...) { bad=true; }
    require(bad,"Invalid padding allowed");
    Detector invalid; VoiceSegmenter is(invalid); invalid.next.completed={{-1,100}};
    bad=false; try { is.accept(frame,512); } catch (...) { bad=true; } require(bad,"Bad boundary accepted");
    Recognizer recognizer; Llm llm; CommandEngine engine(llm); VoiceProcessor processor(recognizer,engine);
    Utterance u{{0,512}, std::vector<float>(512), {}};
    recognizer.text="На кухне поставь свет на 40 процентов";
    auto r=processor.process(u); require(r.action=="L|K|40" && llm.calls==0,"Fast route changed");
    recognizer.text="  Какая погода?  "; r=processor.process(u);
    require(llm.original==recognizer.text && llm.calls==1 && r.action=="U","Transcript modified");
    recognizer.fail=true; recognizer.text="stale"; r=processor.process(u);
    require(r.action=="U" && r.recognition.text.empty() && llm.calls==1,"Failed STT replayed output");
    recognizer.fail=false; recognizer.raises=true; r=processor.process(u);
    require(r.action=="U" && llm.calls==1,"Exception reached LLM");
    recognizer.raises=false; recognizer.text="в коридоре выключи свет";
    r=processor.process(u); require(r.action=="L|C|0","STT did not recover");
    auto calls=recognizer.calls; u.error="too long"; r=processor.process(u);
    require(r.action=="U" && recognizer.calls==calls,"Rejected audio reached STT");
    u.error.clear();u.audio.clear();r=processor.process(u);require(recognizer.calls==calls,"Empty audio reached STT");
    std::cout<<"Voice pipeline tests passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
