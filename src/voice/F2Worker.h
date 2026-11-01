#pragma once
#include "F2Sentences.h"
#include "c-api.h"
#include "nlohmann/json.hpp"
#include <alsa/asoundlib.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace firefly::voice::f2 {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
class Events {
    std::mutex mutex_;
    std::function<void(const std::string&)> sink_;
    Clock::time_point start_=Clock::now();
public:
    explicit Events(std::function<void(const std::string&)> sink):sink_(std::move(sink)){}
    double now()const{return std::chrono::duration<double,std::milli>(Clock::now()-start_).count();}
    void write(std::string event,Json row=Json::object()) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            row["event"]=std::move(event);row["elapsed_ms"]=now();
            if(sink_)sink_(row.dump());
        } catch (...) { /* Diagnostics cannot terminate a speech worker. */ }
    }
};

// Two owned workers: synthesize sentence N+1 while sentence N is played.
// The RKLLM callback only copies text to a bounded queue and never waits for TTS.
class Speaker {
    struct Audio {std::vector<float> samples;int rate;int sequence;};
    Events& events_;
    std::string folder_,device_;
    bool wav_,plain_;
    std::unique_ptr<const SherpaOnnxOfflineTts,decltype(&SherpaOnnxDestroyOfflineTts)> tts_{nullptr,SherpaOnnxDestroyOfflineTts};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> texts_;
    std::deque<Audio> audio_;
    std::unique_ptr<Sentences> parser_;
    std::thread synthesis_,playback_;
    std::atomic<bool> stop_{false},cancelled_{false};
    bool active_=false,finished_=false,synthDone_=false,playDone_=false;
    int turn_=0;
    int sequence_=0;
    std::string error_;
    void fail(std::string error) {
        {std::lock_guard<std::mutex> lock(mutex_);if(error_.empty())error_=std::move(error);cancelled_=true;texts_.clear();audio_.clear();finished_=true;}
        cv_.notify_all();
    }
    static int32_t progress(const float*,int32_t,float,void* arg) {
        auto& self=*static_cast<Speaker*>(arg);
        return !self.stop_&&!self.cancelled_;
    }
    void synthesize() {
        for(;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[&]{return stop_||(active_&&!synthDone_&&(!texts_.empty()||finished_));});
            if(stop_)return;
            if(texts_.empty()){synthDone_=true;cv_.notify_all();continue;}
            auto text=std::move(texts_.front());texts_.pop_front();int turn=turn_;
            lock.unlock();
            try {
                int id=++sequence_;
                events_.write("synthesis_started",{{"turn",turn},{"chunk",id},{"text",text}});
                SherpaOnnxGenerationConfig g{};g.sid=1;g.speed=1;g.num_steps=5;g.extra="{\"lang\":\"ru\"}";g.silence_scale=.2f;
                auto start=Clock::now();
                std::unique_ptr<const SherpaOnnxGeneratedAudio,decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)> a(SherpaOnnxOfflineTtsGenerateWithConfig(tts_.get(),text.c_str(),&g,progress,this),SherpaOnnxDestroyOfflineTtsGeneratedAudio);
                if(stop_)return;
                if(cancelled_)continue;
                if(!a||!a->samples||a->n<=0||a->sample_rate!=44100||a->n>60*a->sample_rate)throw std::runtime_error("Invalid TTS waveform");
                float peak=0;for(int i=0;i<a->n;++i){if(!std::isfinite(a->samples[i]))throw std::runtime_error("Nonfinite TTS PCM");peak=std::max(peak,std::abs(a->samples[i]));}
                // Attenuate overshoot only; never amplify a quiet voice.
                float gain=peak>.96f?.96f/peak:1.f;
                Audio output{{a->samples,a->samples+a->n},a->sample_rate,id};
                for(auto& x:output.samples)x*=gain;
                events_.write("audio_ready",{{"turn",turn},{"chunk",id},{"synthesis_ms",std::chrono::duration<double,std::milli>(Clock::now()-start).count()},
                    {"audio_s",double(a->n)/a->sample_rate},{"gain",gain},{"voice","F2"},{"steps",5}});
                lock.lock();cv_.wait(lock,[&]{return stop_||cancelled_||audio_.size()<2;});
                if(stop_)return;
                if(!cancelled_)audio_.push_back(std::move(output));
                lock.unlock();cv_.notify_all();
            }catch(const std::exception& e){if(lock.owns_lock())lock.unlock();fail(e.what());}
        }
    }
    void play() {
        snd_pcm_t* handle=nullptr;
        int turn=-1;bool started=false;int underruns=0;
        const auto close=[&]{if(handle){snd_pcm_drop(handle);snd_pcm_close(handle);handle=nullptr;}};
        for(;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[&]{return stop_||(active_&&!playDone_&&(!audio_.empty()||synthDone_||cancelled_));});
            if(stop_){close();return;}
            if(turn!=turn_){turn=turn_;started=false;underruns=0;}
            if(cancelled_){close();audio_.clear();playDone_=true;cv_.notify_all();continue;}
            if(audio_.empty()&&synthDone_) {
                lock.unlock();
                if(handle){
                    auto deadline=Clock::now()+std::chrono::seconds(5);int rc=0;
                    while(!stop_&&!cancelled_&&(rc=snd_pcm_drain(handle))==-EAGAIN&&Clock::now()<deadline)snd_pcm_wait(handle,50);
                    if(!stop_&&!cancelled_) {
                        if(rc<0)fail("ALSA drain failed: "+std::string(snd_strerror(rc)));
                        else if(snd_pcm_state(handle)==SND_PCM_STATE_DRAINING)fail("ALSA drain deadline");
                    }
                }
                close();lock.lock();playDone_=true;cv_.notify_all();continue;
            }
            Audio audio=std::move(audio_.front());audio_.pop_front();lock.unlock();cv_.notify_all();
            try {
                if(wav_) {
                    std::string path=folder_+"/turn-"+std::to_string(turn)+"-chunk-"+std::to_string(audio.sequence)+".wav";
                    if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Unsafe WAV output");
                    if(!SherpaOnnxWriteWave(audio.samples.data(),audio.samples.size(),audio.rate,path.c_str()))throw std::runtime_error("Cannot write speech WAV");
                    events_.write("wav_written",{{"turn",turn},{"chunk",audio.sequence},{"path",path}});continue;
                }
                if(!handle) {
                    int rc=snd_pcm_open(&handle,device_.c_str(),SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK);
                    if(rc<0)throw std::runtime_error("Cannot open playback: "+std::string(snd_strerror(rc)));
                    rc=snd_pcm_set_params(handle,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,1,audio.rate,1,150000);
                    if(rc<0)throw std::runtime_error("Cannot configure playback: "+std::string(snd_strerror(rc)));
                }
                std::vector<int16_t> pcm;pcm.reserve(audio.samples.size());
                for(float x:audio.samples)pcm.push_back(static_cast<int16_t>(std::lround(std::clamp(x,-.999f,.999f)*32767)));
                size_t offset=0;auto deadline=Clock::now()+std::chrono::seconds(75);
                while(offset<pcm.size()&&!stop_&&!cancelled_) {
                    if(Clock::now()>deadline)throw std::runtime_error("ALSA write deadline");
                    auto n=snd_pcm_writei(handle,pcm.data()+offset,pcm.size()-offset);
                    if(n>0){offset+=static_cast<size_t>(n);if(!started){events_.write("first_playback",{{"turn",turn}});started=true;}}
                    else if(n==-EAGAIN||n==0)snd_pcm_wait(handle,50);
                    else if(n==-EINTR)continue;
                    else if(n==-EPIPE&&++underruns<=5){snd_pcm_prepare(handle);events_.write("underrun",{{"turn",turn}});}
                    else throw std::runtime_error("ALSA write failure: "+std::string(snd_strerror(n)));
                }
                events_.write("chunk_written",{{"turn",turn},{"chunk",audio.sequence}});
            }catch(const std::exception& e){close();fail(e.what());}
        }
    }
public:
    Speaker(Events& events,std::string model,std::string folder,std::string device,bool wav,bool plain=false):events_(events),folder_(std::move(folder)),device_(std::move(device)),wav_(wav),plain_(plain) {
        if(wav_) {
            std::filesystem::create_directory(folder_);
            if(std::filesystem::is_symlink(folder_))throw std::runtime_error("Unsafe speech directory");
        }
        std::vector<std::string> paths;
        for(auto n:{"duration_predictor.int8.onnx","text_encoder.int8.onnx","vector_estimator.int8.onnx","vocoder.int8.onnx","tts.json","unicode_indexer.bin","voice.bin"})paths.push_back(model+"/"+n);
        SherpaOnnxOfflineTtsConfig c{};c.model.num_threads=4;c.model.provider="cpu";c.max_num_sentences=1;c.silence_scale=.2f;
        c.model.supertonic={paths[0].c_str(),paths[1].c_str(),paths[2].c_str(),paths[3].c_str(),paths[4].c_str(),paths[5].c_str(),paths[6].c_str()};
        auto start=Clock::now();tts_.reset(SherpaOnnxCreateOfflineTts(&c));
        if(!tts_)throw std::runtime_error("TTS initialization failed");
        events_.write("tts_loaded",{{"voice","F2"},{"sid",1},{"threads",4},{"steps",5},{"load_ms",std::chrono::duration<double,std::milli>(Clock::now()-start).count()}});
        synthesis_=std::thread([this]{synthesize();});
        try{playback_=std::thread([this]{play();});}catch(...){stop_=true;cv_.notify_all();synthesis_.join();throw;}
    }
    ~Speaker(){stop_=true;cv_.notify_all();if(synthesis_.joinable())synthesis_.join();if(playback_.joinable())playback_.join();}
    void begin(int turn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(active_)throw std::runtime_error("Previous speech turn still active");
        turn_=turn;finished_=synthDone_=playDone_=false;cancelled_=false;error_.clear();texts_.clear();audio_.clear();
        parser_=std::make_unique<Sentences>([this](std::string text){if(texts_.size()>=32)throw std::runtime_error("Speech queue full");texts_.push_back(std::move(text));cv_.notify_all();},plain_);
        active_=true;
    }
    void push(const std::string& bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!cancelled_&&active_&&!finished_) {
            try{parser_->push(bytes);}
            catch(const std::exception& e){error_=e.what();cancelled_=true;texts_.clear();audio_.clear();finished_=true;cv_.notify_all();}
        }
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_=true;finished_=true;texts_.clear();audio_.clear();cv_.notify_all();
    }
    std::string error() { std::lock_guard<std::mutex> lock(mutex_);return error_; }
    bool cancelled() const { return cancelled_.load(); }
    bool finish(bool success, const std::function<bool()>& interrupted = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        if(success&&!cancelled_) {
            try{parser_->finish();}
            catch(const std::exception& e){error_=e.what();cancelled_=true;texts_.clear();audio_.clear();}
        }
        else {cancelled_=true;texts_.clear();audio_.clear();}
        finished_=true;cv_.notify_all();
        auto deadline=Clock::now()+std::chrono::seconds(60);
        while(!(synthDone_&&playDone_)) {
            bool stop=false;
            try {stop=interrupted&&interrupted();} catch (...) {stop=true;}
            if(stop||Clock::now()>=deadline) {
                if(Clock::now()>=deadline&&error_.empty())error_="speech_timeout";
                cancelled_=true;texts_.clear();audio_.clear();finished_=true;cv_.notify_all();
            }
            cv_.wait_for(lock,std::chrono::milliseconds(25));
        }
        active_=false;
        events_.write("speech_finished",{{"turn",turn_},{"ok",error_.empty()&&success&&!cancelled_},{"error",error_}});
        return error_.empty()&&success&&!cancelled_;
    }
};

}
