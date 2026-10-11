#pragma once
#include "firefly/admin/RuntimeGeneration.h"
#include <string>
#include <chrono>
#include "firefly/voice/VoiceProcessor.h"
namespace firefly::admin {
std::string buildIdentityJson();
class RuntimeWorker final {
public:
    explicit RuntimeWorker(ContextModel& model) : model_(model), generations_(model) {}
    std::string handle(std::string_view message);
    std::string observe() const;
    voice::VoiceResult processVoice(voice::SpeechRecognizer& recognizer, const voice::Utterance& utterance);
    void voiceStatus(std::string state, std::string microphone);
    void stop() { generations_.stop(); }
    bool setupBlocked() const { return generations_.setupBlocked(); }
    void setAudioDrain(std::function<void()> drain) { generations_.setAudioDrain(std::move(drain)); }
private:
    void remember(std::string diagnostic);
    mutable std::mutex diagnostics_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::string lastVoice_;
    std::string lastCommand_, voiceState_ = "disabled", microphone_ = "disabled";
private:
    ContextModel& model_;
    RuntimeGenerations generations_;
};
class RuntimeClient {
public:
    virtual ~RuntimeClient() = default;
    virtual std::string call(std::string_view message) = 0;
    // In-process read-only diagnostics remain available during inference. Empty
    // means legacy IPC client: use its existing cached status behavior.
    virtual std::string observe() const { return {}; }
    // Local setup closes admission before waiting for a currently accepted
    // operation. Serialized IPC must retain its transport lock.
    virtual bool concurrentSetupEntry() const { return false; }
};
class LocalRuntimeClient final : public RuntimeClient {
public:
    explicit LocalRuntimeClient(RuntimeWorker& worker) : worker_(worker) {}
    std::string call(std::string_view message) override { return worker_.handle(message); }
    std::string observe() const override { return worker_.observe(); }
    bool concurrentSetupEntry() const override { return true; }
private:
    RuntimeWorker& worker_;
};
}
