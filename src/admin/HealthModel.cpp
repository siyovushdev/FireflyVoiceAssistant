#include "firefly/admin/HealthModel.h"
#include <iostream>
#include <nlohmann/json.hpp>

namespace firefly::admin {
namespace {
using Json = nlohmann::json;
}

HealthModel::HealthModel() {
    for (const char* name : {"configuration", "runtime", "admin", "stt", "llm", "hardware_execution"})
        state_.subsystems[name] = {};
    state_.subsystems["admin"].status = SubsystemStatus::Starting;
    state_.subsystems["stt"].status = SubsystemStatus::Unavailable;
    state_.subsystems["llm"].status = SubsystemStatus::Starting;
    state_.subsystems["hardware_execution"].status = SubsystemStatus::Unavailable;
}

void HealthModel::setPhase(std::string phase) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase == phase) return;
    state_.phase = std::move(phase);
    const auto previous = state_.overall;
    state_.overall = computeOverall(state_);
    if (state_.overall != previous) logTransition(previous, state_.overall, state_.phase);
    lastLogged_ = state_.overall;
}

void HealthModel::setStopping(bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.stopping == value) return;
    state_.stopping = value;
    if (value) state_.phase = "stopping";
    const auto previous = state_.overall;
    state_.overall = computeOverall(state_);
    if (state_.overall != previous) logTransition(previous, state_.overall, state_.phase);
    lastLogged_ = state_.overall;
}

void HealthModel::setConfigRecovered(bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.configRecovered = value;
}

void HealthModel::setSubsystem(const std::string& name, SubsystemStatus status,
                               std::string error, std::string detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = state_.subsystems[name];
    if (entry.status == status && entry.error == error && entry.detail == detail) return;
    entry.status = status;
    entry.error = std::move(error);
    entry.detail = std::move(detail);
    const auto previous = state_.overall;
    state_.overall = computeOverall(state_);
    if (state_.overall != previous) logTransition(previous, state_.overall, state_.phase);
    lastLogged_ = state_.overall;
}

HealthSnapshot HealthModel::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void HealthModel::refreshOverall() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto previous = state_.overall;
    state_.overall = computeOverall(state_);
    if (state_.overall != previous) logTransition(previous, state_.overall, state_.phase);
    lastLogged_ = state_.overall;
}

std::string HealthModel::overallName(HealthOverall value) {
    switch (value) {
        case HealthOverall::Starting: return "starting";
        case HealthOverall::Ready: return "ready";
        case HealthOverall::Degraded: return "degraded";
        case HealthOverall::Failed: return "failed";
    }
    return "failed";
}

std::string HealthModel::subsystemName(SubsystemStatus value) {
    switch (value) {
        case SubsystemStatus::Starting: return "starting";
        case SubsystemStatus::Ready: return "ready";
        case SubsystemStatus::Failed: return "failed";
        case SubsystemStatus::Unavailable: return "unavailable";
    }
    return "unavailable";
}

std::string HealthModel::toJson(const HealthSnapshot& snapshot) {
    Json subsystems = Json::object();
    for (const auto& [name, entry] : snapshot.subsystems) {
        Json item{{"status", subsystemName(entry.status)}};
        if (!entry.error.empty()) item["error"] = entry.error;
        if (!entry.detail.empty()) item["detail"] = entry.detail;
        subsystems[name] = item;
    }
    return Json{{"overall", overallName(snapshot.overall)},
        {"phase", snapshot.phase},
        {"stopping", snapshot.stopping},
        {"config_recovered", snapshot.configRecovered},
        {"subsystems", subsystems}}.dump();
}

HealthOverall HealthModel::computeOverall(const HealthSnapshot& value) const {
    if (value.stopping) return HealthOverall::Degraded;
    const auto* configuration = value.subsystems.count("configuration")
        ? &value.subsystems.at("configuration") : nullptr;
    const auto* admin = value.subsystems.count("admin") ? &value.subsystems.at("admin") : nullptr;
    if (admin && admin->status == SubsystemStatus::Failed)
        return HealthOverall::Failed;
    if (configuration && configuration->status == SubsystemStatus::Failed &&
        (!admin || admin->status != SubsystemStatus::Ready))
        return HealthOverall::Failed;
    if (value.phase == "process_started" || value.phase == "configuration_loading" ||
        value.phase == "runtime_initializing" || value.phase == "http_listening")
        return HealthOverall::Starting;

    bool anyFailed = false;
    bool anyStarting = false;
    bool requiredReady = true;
    for (const auto& [name, entry] : value.subsystems) {
        if (entry.status == SubsystemStatus::Failed) anyFailed = true;
        if (entry.status == SubsystemStatus::Starting) anyStarting = true;
        // Required for READY: configuration, runtime, admin. STT/LLM may degrade.
        if ((name == "configuration" || name == "runtime" || name == "admin") &&
            entry.status != SubsystemStatus::Ready)
            requiredReady = false;
    }
    if (value.configRecovered || anyFailed) return HealthOverall::Degraded;
    if (!requiredReady || anyStarting) return HealthOverall::Starting;
    return HealthOverall::Ready;
}

void HealthModel::logTransition(HealthOverall previous, HealthOverall next, const std::string& phase) const {
    std::cerr << "HEALTH " << overallName(previous) << " -> " << overallName(next)
              << " phase=" << phase << '\n';
}
}
