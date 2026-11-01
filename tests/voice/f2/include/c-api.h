#pragma once
#include <cstdint>
// CPU-only TTS dependency double. Production compiles against the real pinned
// sherpa-onnx header/library; native acceptance checks that separate ABI.
extern "C" {
struct SherpaOnnxOfflineTts;
struct SherpaOnnxOfflineTtsSupertonicModelConfig { const char *duration_predictor,*text_encoder,*vector_estimator,*vocoder,*tts_json,*unicode_indexer,*voice_style; };
struct SherpaOnnxOfflineTtsModelConfig { int32_t num_threads; const char* provider; SherpaOnnxOfflineTtsSupertonicModelConfig supertonic; };
struct SherpaOnnxOfflineTtsConfig { SherpaOnnxOfflineTtsModelConfig model; int32_t max_num_sentences; float silence_scale; };
struct SherpaOnnxGenerationConfig { int32_t sid; float speed; int32_t num_steps; const char* extra; float silence_scale; };
struct SherpaOnnxGeneratedAudio { const float* samples; int32_t n,sample_rate; };
using SherpaOnnxGeneratedAudioProgressCallbackWithArg=int32_t(*)(const float*,int32_t,float,void*);
const SherpaOnnxOfflineTts* SherpaOnnxCreateOfflineTts(const SherpaOnnxOfflineTtsConfig*);
void SherpaOnnxDestroyOfflineTts(const SherpaOnnxOfflineTts*);
const SherpaOnnxGeneratedAudio* SherpaOnnxOfflineTtsGenerateWithConfig(const SherpaOnnxOfflineTts*,const char*,const SherpaOnnxGenerationConfig*,SherpaOnnxGeneratedAudioProgressCallbackWithArg,void*);
void SherpaOnnxDestroyOfflineTtsGeneratedAudio(const SherpaOnnxGeneratedAudio*);
int32_t SherpaOnnxWriteWave(const float*,int32_t,int32_t,const char*);
}
