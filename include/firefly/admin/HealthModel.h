#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace firefly::admin {
// Process-level health. Independent of ExecutionStatus / DeviceState axes (M15).
enum class HealthOverall { Starting, Ready, Degraded, Failed };
enum class SubsystemStatus { Starting, Ready, Failed, Unavailable };

struct SubsystemHealth {
    SubsystemStatus status = SubsystemStatus::Starting;
    std::string error;   // short machine-friendly code or message
    std::string detail;  // optional human diagnostic
};

struct HealthSnapshot {
    HealthOverall overall = HealthOverall::Starting;
    std::string phase = "process_started";
    bool stopping = false;
    bool configRecovered = false;
    std::map<std::string, SubsystemHealth> subsystems;
};

// Thread-safe mutable health owned by Api / production main.
class HealthModel final {
public:
    HealthModel();

    void setPhase(std::string phase);
    void setStopping(bool value);
    void setConfigRecovered(bool value);
    void setSubsystem(const std::string& name, SubsystemStatus status,
                      std::string error = {}, std::string detail = {});

    HealthSnapshot snapshot() const;
    // Recalculate overall from subsystems + stopping + phase. Logs only on change.
    void refreshOverall();

    static std::string overallName(HealthOverall value);
    static std::string subsystemName(SubsystemStatus value);
    static std::string toJson(const HealthSnapshot& snapshot);

private:
    HealthOverall computeOverall(const HealthSnapshot& value) const;
    void logTransition(HealthOverall previous, HealthOverall next, const std::string& phase) const;

    mutable std::mutex mutex_;
    HealthSnapshot state_;
    HealthOverall lastLogged_ = HealthOverall::Starting;
};
}
