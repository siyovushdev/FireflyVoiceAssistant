#pragma once
#include "firefly/voice/Speech.h"
#include <memory>
namespace firefly::voice {
class SherpaVad final : public SpeechDetector {
public:
    explicit SherpaVad(const std::string& model);
    ~SherpaVad() override;
    Detection accept(const float*, int) override;
    void reset() override;
    int startConfirmationMs() const override { return 288; } // 250 ms + one 32 ms frame, rounded up.
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
class SherpaRecognizer final : public SpeechRecognizer {
public:
    explicit SherpaRecognizer(const std::string& modelDirectory);
    ~SherpaRecognizer() override;
    RecognitionResult recognize(const std::vector<float>&) override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
