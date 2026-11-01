#pragma once
// Compiled only into the uninstalled native acceptance executable. PCM is paced
// at 16 kHz; the shared cursor survives audio recovery and cannot replay a turn.
#include "firefly/voice/VoiceLoop.h"
#include "c-api.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <cstdlib>
#include <stdexcept>
namespace firefly::voice::test {
struct ReplayData {
    std::vector<float> pcm;
    std::size_t offset=0;
    explicit ReplayData(const std::string& path) {
        std::unique_ptr<const SherpaOnnxWave,decltype(&SherpaOnnxFreeWave)> w(SherpaOnnxReadWave(path.c_str()),SherpaOnnxFreeWave);
        if(!w||w->sample_rate!=16000||w->num_samples<=0||w->num_samples>16000*1200)throw std::runtime_error("invalid native replay fixture");
        pcm.assign(w->samples,w->samples+w->num_samples);
    }
};
class ReplayInput final : public AudioSource {
    std::shared_ptr<ReplayData> data_;
public:
    explicit ReplayInput(std::shared_ptr<ReplayData> data):data_(std::move(data)){}
    int read(float* out,int capacity)override {
        std::this_thread::sleep_for(std::chrono::microseconds(1000000*capacity/16000));
        for(int i=0;i<capacity;++i)out[i]=data_->offset<data_->pcm.size()?data_->pcm[data_->offset++]:0;
        return capacity;
    }
};
}
