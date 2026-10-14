#include "firefly/SetupResources.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace firefly {
namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("memory_status_unavailable");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::uint64_t number(const std::string& text) {
    std::istringstream input(text);
    std::uint64_t value = 0;
    input >> std::ws;
    if (input.peek() < '0' || input.peek() > '9' || !(input >> value))
        throw std::runtime_error("memory_status_unavailable");
    return value;
}
std::uint64_t field(const std::string& text, const std::string& key) {
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
        if (line.rfind(key + ':', 0) == 0) return number(line.substr(key.size() + 1));
    throw std::runtime_error("memory_status_unavailable");
}
}
SetupResources::SetupResources(Hooks hooks) : hooks_(std::move(hooks)) {
    if (!hooks_.read) hooks_.read = [] { return readLinux(); };
    if (!hooks_.now) hooks_.now = std::chrono::steady_clock::now;
    if (!hooks_.wait) hooks_.wait = [](auto delay) { std::this_thread::sleep_for(delay); };
    if (!hooks_.log) hooks_.log = [](const auto& message) { std::cerr << message << '\n'; };
}
std::unique_lock<std::timed_mutex> SetupResources::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!lock.try_lock_for(timeout)) throw std::runtime_error("admin_resource_timeout");
    return lock;
}
SetupResources::Memory SetupResources::checkpoint(const std::string& phase, std::uint64_t minimumKiB) {
    Memory memory;
    try { memory = hooks_.read(); }
    catch (...) {
        hooks_.log("SETUP_MEMORY phase=" + phase + " error=memory_status_unavailable");
        throw std::runtime_error("memory_status_unavailable");
    }
    std::ostringstream line;
    line << "SETUP_MEMORY phase=" << phase << " available_kib=" << memory.availableKiB
         << " pss_kib=" << memory.pssKiB << " dma_bytes=" << memory.dmaBytes
         << " cgroup_current_bytes=" << memory.cgroupCurrentBytes
         << " cgroup_headroom_kib=" << memory.cgroupHeadroomKiB << " required_kib=" << minimumKiB;
    hooks_.log(line.str());
    if (memory.availableKiB < minimumKiB || memory.cgroupHeadroomKiB < minimumKiB)
        throw std::runtime_error("admin_memory_reserve_unavailable");
    return memory;
}
void SetupResources::note(const std::string& phase) noexcept {
    try { checkpoint(phase); }
    catch (...) { try { hooks_.log("SETUP_MEMORY phase=" + phase + " error=memory_status_unavailable"); } catch (...) {} }
}
void SetupResources::awaitReleased(std::uint64_t baselineDmaBytes) {
    const auto deadline = hooks_.now() + std::chrono::seconds(2);
    while (hooks_.read().dmaBytes > baselineDmaBytes) {
        if (hooks_.now() >= deadline) {
            note("release_timeout");
            throw std::runtime_error("model_release_timeout");
        }
        hooks_.wait(std::chrono::milliseconds(50));
    }
    note("release_confirmed");
}
void SetupResources::awaitAdmission(std::uint64_t minimumKiB) {
    note("admin_admission_wait");
    const auto deadline = hooks_.now() + std::chrono::seconds(2);
    for (;;) {
        const auto memory = hooks_.read();
        if ((memory.availableKiB >= minimumKiB && memory.cgroupHeadroomKiB >= minimumKiB) || hooks_.now() >= deadline)
            break;
        hooks_.wait(std::chrono::milliseconds(50));
    }
    // Never authorize from a stale observation, and retain the shortage error
    // when a settled system simply cannot meet the unchanged budget.
    checkpoint("admin_admission", minimumKiB);
}
SetupResources::Memory SetupResources::readLinux(const std::string& proc, const std::string& cgroup) {
    namespace fs = std::filesystem;
    Memory memory;
    memory.availableKiB = field(read(fs::path(proc) / "meminfo"), "MemAvailable");
    memory.pssKiB = field(read(fs::path(proc) / "self/smaps_rollup"), "Pss");
    // fdinfo attributes identify DMA buffers; ordinary DRM device fds have no size.
    for (const auto& file : fs::directory_iterator(fs::path(proc) / "self/fdinfo")) {
        std::ifstream input(file.path()); // Unrelated fds may disappear concurrently.
        std::string content{std::istreambuf_iterator<char>(input), {}};
        if (content.find("exp_name:\tdrm") != std::string::npos)
            memory.dmaBytes += field(content, "size");
    }
    std::istringstream groups(read(fs::path(proc) / "self/cgroup"));
    std::string line, relative;
    bool unified = false;
    while (std::getline(groups, line)) if (line.rfind("0::/", 0) == 0) { unified = true; relative = line.substr(4); }
    // This appliance uses cgroup v2. An unknown hierarchy cannot authorize a load.
    if (!unified) throw std::runtime_error("memory_status_unavailable");
    for (const auto& component : fs::path(relative))
        if (component == "..") throw std::runtime_error("memory_status_unavailable");
    const fs::path root(cgroup);
    fs::path directory = relative.empty() ? root : root / relative;
    bool leaf = true;
    while (directory != root) {
        const auto current = number(read(directory / "memory.current"));
        if (leaf) memory.cgroupCurrentBytes = current;
        for (const auto* name : {"memory.max", "memory.high"}) {
            const auto limit = read(directory / name);
            if (limit.rfind("max", 0) != 0) {
                const auto bytes = number(limit);
                memory.cgroupHeadroomKiB = std::min(memory.cgroupHeadroomKiB, bytes > current ? (bytes - current) / 1024 : 0);
            }
        }
        leaf = false;
        directory = directory.parent_path();
    }
    return memory;
}
}
