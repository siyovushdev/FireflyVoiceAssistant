// Linked ONLY into voice_recovery_fixture. No ALSA/sherpa/vendor libraries,
// real devices, model files, or child processes. DemoMain and voice loops are real.
#include "firefly/voice/AlsaAudioSource.h"
#include "firefly/voice/AminaWakeDetector.h"
#include "firefly/voice/SherpaSpeech.h"
#include "firefly/voice/WakeAcknowledgement.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace firefly::voice {
namespace {
std::string mode() {
    const auto* path = std::getenv("FIREFLY_TEST_VOICE_CONTROL");
    if (!path) throw std::runtime_error("Missing fixture control path");
    std::ifstream input(path);
    std::string value;
    input >> value;
    return value;
}
int captureOwners = 0;
}
struct SherpaVad::Impl {};
SherpaVad::SherpaVad(const std::string&) : impl_(std::make_unique<Impl>()) {
    std::cerr << "FIXTURE VAD loaded\n";
}
SherpaVad::~SherpaVad() { std::cerr << "FIXTURE VAD released\n"; }
Detection SherpaVad::accept(const float*, int) { return {}; }
void SherpaVad::reset() { std::cerr << "FIXTURE VAD reset\n"; }
struct SherpaRecognizer::Impl { RecognitionProfile profile = RecognitionProfile::Command; };
SherpaRecognizer::SherpaRecognizer(const std::string&, RecognitionProfile profile) : impl_(std::make_unique<Impl>()) {
    if (profile == RecognitionProfile::Command && mode() == "stt-failure") throw std::runtime_error("fixture STT load failure");
    impl_->profile = profile;
    std::cerr << "FIXTURE STT loaded profile=" << (profile == RecognitionProfile::Command ? "command" : "admin") << '\n';
}
SherpaRecognizer::~SherpaRecognizer() {
    std::cerr << "FIXTURE STT released profile=" << (impl_->profile == RecognitionProfile::Command ? "command" : "admin") << '\n';
}
RecognitionResult SherpaRecognizer::recognize(const std::vector<float>&) { return {}; }
struct AlsaAudioSource::Impl {};
AlsaAudioSource::AlsaAudioSource(const std::string&, unsigned, unsigned) {
    if (mode() == "absent") throw std::runtime_error("Cannot open ALSA capture fixture: No such device");
    if (captureOwners) throw std::logic_error("Concurrent capture owner");
    impl_ = std::make_unique<Impl>();
    ++captureOwners;
    std::cerr << "FIXTURE capture opened owners=" << captureOwners << '\n';
}
AlsaAudioSource::~AlsaAudioSource() {
    --captureOwners;
    std::cerr << "FIXTURE capture closed owners=" << captureOwners << '\n';
}
int AlsaAudioSource::read(float* samples, int count) {
    const auto current = mode();
    if (current == "absent" || current == "read-failure")
        throw std::runtime_error("ALSA capture read failed: fixture disconnect");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (current == "stall") return 0;
    std::fill_n(samples, count, 0);
    return count;
}
struct AminaWakeDetector::Impl { bool detected = false; };
AminaWakeDetector::AminaWakeDetector(const WakeConfig&, const std::string&, const std::string&)
    : impl_(std::make_unique<Impl>()) {
    if (captureOwners) throw std::logic_error("Wake initialization while capture is already running");
    if (mode() == "wake-failure") throw std::runtime_error("fixture wake worker failed");
    std::cerr << "FIXTURE wake created\n";
}
AminaWakeDetector::~AminaWakeDetector() { std::cerr << "FIXTURE wake destroyed\n"; }
bool AminaWakeDetector::accept(const float*, int) {
    if (mode().rfind("ack-", 0) == 0 && !impl_->detected) {
        impl_->detected = true;
        return true;
    }
    return false;
}
void AminaWakeDetector::reset() {}
struct AlsaWakeAcknowledgement::Impl {};
AlsaWakeAcknowledgement::AlsaWakeAcknowledgement(std::string, WakeAcknowledgementConfig config)
    : impl_(std::make_unique<Impl>()) { config.validate(); }
AlsaWakeAcknowledgement::~AlsaWakeAcknowledgement() = default;
void AlsaWakeAcknowledgement::start() { std::cerr << "FIXTURE beep started\n"; }
void AlsaWakeAcknowledgement::cancel() noexcept {}
AcknowledgementStatus AlsaWakeAcknowledgement::poll() {
    return mode() == "ack-failure" ? AcknowledgementStatus::Failed : AcknowledgementStatus::Complete;
}
std::string AlsaWakeAcknowledgement::error() const { return "fixture playback failure"; }
}
