#pragma once
#include <functional>
#include <memory>
#include <string>
namespace firefly::voice {
struct SpeechOutcome { bool success = false, cancelled = false; std::string error; };
class F2SpeechOutput final {
public:
    using Events = std::function<void(const std::string&)>;
    using LoadGuard = std::function<void(const std::function<void()>&)>;
    // Existing Supertonic3 INT8 F2/SID1, CPU4/steps5/speed1. No microphone/NPU.
    F2SpeechOutput(const std::string& modelDirectory, const std::string& playbackDevice,
                   Events events = {}, const std::string& diagnosticWavDirectory = {},
                   LoadGuard loadGuard = {});
    ~F2SpeechOutput();
    SpeechOutcome speak(const std::string& text, const std::function<bool()>& interrupted);
    void cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
