#pragma once
#include <memory>
#include <string>
namespace firefly::voice {
enum class AcknowledgementStatus { Playing, Complete, Failed };
// Nonblocking, owned by the capture loop. No worker or subprocess. Failure is
// diagnostic only: admission still requires wake and a healthy capture/VAD/STT.
class WakeAcknowledgement {
public:
    virtual ~WakeAcknowledgement() = default;
    virtual void start() = 0;
    virtual AcknowledgementStatus poll() = 0;
    virtual void cancel() noexcept = 0;
    virtual std::string error() const { return {}; }
};
class AlsaWakeAcknowledgement final : public WakeAcknowledgement {
public:
    explicit AlsaWakeAcknowledgement(std::string device);
    ~AlsaWakeAcknowledgement() override;
    void start() override;
    AcknowledgementStatus poll() override;
    void cancel() noexcept override;
    std::string error() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
