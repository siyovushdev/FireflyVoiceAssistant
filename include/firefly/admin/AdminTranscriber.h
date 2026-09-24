#pragma once
#include "firefly/voice/Speech.h"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace firefly::admin {
// Long-lived Admin Copilot STT owner. Browser uploads only — never feeds CommandEngine.
class AdminTranscriber final {
public:
    explicit AdminTranscriber(std::unique_ptr<voice::SpeechRecognizer> recognizer);
    bool ready() const;
    // Returns JSON object body. Fail-closed on malformed/oversized/busy/empty speech.
    std::string transcribe(std::string_view wavBytes);

private:
    std::unique_ptr<voice::SpeechRecognizer> recognizer_;
    mutable std::mutex mutex_;
};
}
