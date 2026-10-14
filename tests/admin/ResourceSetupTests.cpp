#include "RuntimeModel.h"
#include "Support.h"
#include "FakeRkllmRuntime.h"
#include "firefly/admin/AdminTranscriber.h"
#include <future>
#include <algorithm>
#include <iostream>
#include <sys/wait.h>

using firefly::SetupResources;
using firefly::admin::RuntimeModel;
using firefly::admin::RuntimeGenerations;
using firefly_test::stub;
using namespace std::chrono_literals;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> std::string error(F&& operation) {
    try { operation(); } catch (const std::exception& e) { return e.what(); }
    return {};
}
struct Rig {
    SetupResources::Memory memory{7 * SetupResources::GiBKiB, 0, 0, 0};
    std::chrono::steady_clock::time_point now{};
    std::vector<std::string> logs;
    std::shared_ptr<SetupResources> resources = std::make_shared<SetupResources>(SetupResources::Hooks{
        [&] { return memory; }, [&] { return now; }, [&](auto delay) { now += delay; },
        [&](const auto& line) { logs.push_back(line); }});
    Rig() {
        stub.onInit = [&] { memory.dmaBytes = 4096; memory.availableKiB = 2 * SetupResources::GiBKiB; };
        stub.onDestroy = [&] { memory.dmaBytes = 0; memory.availableKiB = 7 * SetupResources::GiBKiB; };
    }
    ~Rig() { stub.onInit = {}; stub.onDestroy = {}; stub.onRun = {}; stub.failInit = false; stub.failDestroy = false; }
    bool logged(const std::string& phase) const {
        return std::any_of(logs.begin(), logs.end(), [&](const auto& line) { return line.find("phase=" + phase + " ") != std::string::npos; });
    }
};
void budgetPolicy() {
    Rig r;
    r.resources->checkpoint("admit", 6 * SetupResources::GiBKiB);
    r.memory.availableKiB = 6 * SetupResources::GiBKiB - 1;
    check(error([&] { r.resources->checkpoint("low", 6 * SetupResources::GiBKiB); }) == "admin_memory_reserve_unavailable", "6 GiB remains exact");
    r.memory.availableKiB = 7 * SetupResources::GiBKiB;
    r.memory.cgroupHeadroomKiB = SetupResources::GiBKiB - 1;
    check(error([&] { r.resources->checkpoint("limited", SetupResources::GiBKiB); }) == "admin_memory_reserve_unavailable", "cgroup headroom enforced despite global RAM");
    r.memory.dmaBytes = 4096;
    check(error([&] { r.resources->awaitReleased(0); }) == "model_release_timeout", "DMA release deadline distinct");
    check(r.now.time_since_epoch() == 2s, "release wait is bounded");
    r.memory.dmaBytes = 0;
    r.resources->awaitReleased(0);
    check(r.logged("release_confirmed"), "release diagnostics");
    check(error([&] { r.resources->awaitAdmission(6 * SetupResources::GiBKiB); }) == "admin_memory_reserve_unavailable", "settled cgroup shortage is not a release timeout");
    check(r.now.time_since_epoch() == 4s, "admission wait also bounded");
    auto held = r.resources->acquire();
    auto contender = std::async(std::launch::async, [&] { return error([&] { r.resources->acquire(1ms); }); });
    check(contender.get() == "admin_resource_timeout", "STT/model resource wait bounded");
}
void delayedReclaim() {
    SetupResources::Memory memory{5 * SetupResources::GiBKiB, 0, 0, 0};
    std::chrono::steady_clock::time_point now{};
    int waits = 0, records = 0;
    SetupResources resources({[&] { return memory; }, [&] { return now; }, [&](auto delay) {
        now += delay;
        if (++waits == 3) memory.availableKiB = 6 * SetupResources::GiBKiB;
    }, [&](const auto&) { ++records; }});
    resources.awaitAdmission(6 * SetupResources::GiBKiB);
    check(waits == 3 && records == 2, "waits for observed reclaim without lowering threshold or flooding logs");
}
void linuxAccounting() {
    admin_test::Directory directory;
    const auto base = std::filesystem::path(directory.path);
    auto write = [&](const std::string& path, const std::string& text) {
        std::filesystem::create_directories((base / path).parent_path());
        std::ofstream(base / path) << text;
    };
    write("proc/meminfo", "MemAvailable: 7340032 kB\n");
    write("proc/self/smaps_rollup", "Pss: 123 kB\n");
    write("proc/self/cgroup", "0::/parent/child\n");
    write("proc/self/fdinfo/4", "size:\t4096\nexp_name:\tdrm\n");
    write("proc/self/fdinfo/5", "pos: 0\n");
    for (const auto* group : {"parent", "parent/child"}) {
        write(std::string("cg/") + group + "/memory.current", "1048576\n");
        write(std::string("cg/") + group + "/memory.max", "max\n");
        write(std::string("cg/") + group + "/memory.high", "max\n");
    }
    write("cg/parent/memory.max", "2097152\n");
    const auto sample = SetupResources::readLinux((base / "proc").string(), (base / "cg").string());
    check(sample.pssKiB == 123 && sample.dmaBytes == 4096, "PSS and DMA units separated");
    check(sample.cgroupHeadroomKiB == 1024 && sample.cgroupCurrentBytes == 1048576, "ancestor cgroup limit minus actual use");
    write("proc/meminfo", "broken\n");
    check(error([&] { SetupResources::readLinux((base / "proc").string(), (base / "cg").string()); }) == "memory_status_unavailable", "missing evidence fails closed");
}
void transitions(const std::string& root) {
    Rig r;
    RuntimeModel model("fake-runtime", "fake-admin", r.resources, [](const auto&) {});
    RuntimeGenerations runtime(model);
    runtime.apply(admin_test::catalog(admin_test::base(root)));
    const auto original = runtime.current();
    int drains = 0;
    runtime.setAudioDrain([&] { ++drains; });
    for (int cycle = 0; cycle < 2; ++cycle) {
        runtime.enterSetup();
        check(runtime.setupMode() == RuntimeGenerations::SetupMode::Active, "exclusive Admin enters");
        check(!error([&] { runtime.process("Включи свет на кухне", firefly::execution::ExecutionIntent::Production); }).empty(), "Setup blocks hardware command admission");
        model.useAdminContext("admin task", 128);
        check(!model.inferAdmin("request").empty(), "AI on the real adapter with fake C API");
        runtime.prepareRestore();
        check(runtime.setupBlocked(), "command admission stays closed during restoration");
        runtime.finishRestore();
        check(runtime.modelReady() && !runtime.setupBlocked(), "restored command model ready");
        check(runtime.current() == original && runtime.history().empty(), "configuration identity preserved; no executions");
    }
    check(drains == 4 && stub.peakHandles == 1 && stub.activeHandles == 1, "one model owner throughout repeated cycles");
    for (const auto* phase : {"before_destroy", "after_destroy", "admin_before_init", "admin_after_init", "admin_before_bootstrap", "admin_after_bootstrap", "admin_before_inference", "admin_after_inference", "before_restore", "after_restore"})
        check(r.logged(phase), "phase diagnostics complete");
    const auto inits = stub.init;
    firefly::RkllmConfig config; config.modelPath = "second"; config.systemPrompt = "frozen";
    check(error([&] { firefly::RkllmEngine second(config); }) == "rkllm_owner_busy" && stub.init == inits, "second owner rejected before vendor allocation");
}
void failures(const std::string& root) {
    for (const auto* failure : {"memory", "verify", "init", "bootstrap", "post_init_memory", "post_bootstrap_memory", "release", "drain", "restore"}) {
        Rig r;
        const std::string kind(failure);
        RuntimeModel model("fake-runtime", "fake-admin", r.resources, [&](const auto&) {
            if (kind == "verify") throw std::runtime_error("admin_model_sha256_mismatch");
            if (kind == "init") stub.failInit = true;
            if (kind == "bootstrap") stub.fault = firefly_test::Fault::Run;
        });
        RuntimeGenerations runtime(model);
        runtime.apply(admin_test::catalog(admin_test::base(root)));
        const auto original = runtime.current();
        bool failNextInitMemory = true;
        if (kind == "post_bootstrap_memory") stub.onRun = [&] { r.memory.availableKiB = 0; };
        if (kind == "memory") stub.onDestroy = [&] { r.memory.dmaBytes = 0; r.memory.availableKiB = 5 * SetupResources::GiBKiB; };
        if (kind == "release") stub.onDestroy = [] {};
        if (kind == "drain") runtime.setAudioDrain([] { throw std::runtime_error("audio_drain_timeout"); });
        if (kind == "post_init_memory") stub.onInit = [&] {
            r.memory.availableKiB = failNextInitMemory ? 0 : 2 * SetupResources::GiBKiB;
            failNextInitMemory = false;
        };
        const auto beforeInit = stub.init;
        if (kind == "restore") {
            runtime.enterSetup(); stub.failInit = true;
            check(error([&] { runtime.prepareRestore(); }) == "runtime_restore_failed", "restore failure classified");
        } else {
            const auto message = error([&] { runtime.enterSetup(); });
            check(!message.empty(), "injected transition failure reported");
            if (kind == "release") check(message == "model_release_timeout", "release failure retains cause");
            if (kind == "memory") check(message == "admin_memory_reserve_unavailable", "admission failure retains cause");
            if (kind == "drain" || kind == "release")
                check(stub.init == beforeInit && runtime.setupBlocked(), "uncertain teardown never starts a replacement");
        }
        if (runtime.setupBlocked())
            check(!error([&] { runtime.process("Включи свет на кухне", firefly::execution::ExecutionIntent::Production); }).empty(), "failed recovery remains fail closed");
        check(runtime.current() == original && runtime.history().empty(), "failure does not replace config or execute hardware");
        stub.failDestroy = false; stub.failInit = false; stub.fault = firefly_test::Fault::None;
    }
}
void failedDestroyIsTerminal(const std::string& root) {
    // The process-wide owner intentionally remains poisoned. Isolate this
    // case instead of introducing a production reset/bypass for a unit test.
    const auto child = ::fork();
    check(child >= 0, "fork terminal teardown fixture");
    if (child == 0) {
        try {
            Rig r;
            const auto before = stub.destroyAttempts;
            {
                RuntimeModel model("fake-runtime", "fake-admin", r.resources, [](const auto&) {});
                RuntimeGenerations runtime(model);
                runtime.apply(admin_test::catalog(admin_test::base(root)));
                stub.failDestroy = true;
                check(!error([&] { runtime.enterSetup(); }).empty() && runtime.setupBlocked(), "destroy failure closes admission");
                stub.failDestroy = false; // Even a now-successful stub must never be retried.
                check(!error([&] { model.leaveSetup("frozen"); }).empty(), "uncertain handle requires restart");
            }
            check(stub.destroyAttempts == before + 1 && stub.activeHandles == 1, "exactly one destroy attempt, including destructor");
            firefly::RkllmConfig config; config.modelPath = "new"; config.systemPrompt = "frozen";
            check(error([&] { firefly::RkllmEngine replacement(config); }) == "rkllm_owner_busy", "failed teardown retains the process owner");
            ::_exit(0);
        } catch (...) { ::_exit(1); }
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "terminal destroy failure isolated without a second owner");
}
void guardedInference() {
    Rig r;
    RuntimeModel model("fake-runtime", "fake-admin", r.resources, [](const auto&) {});
    model.useContext("frozen"); model.enterSetup();
    const auto runs = stub.run;
    r.memory.availableKiB = SetupResources::GiBKiB - 1;
    check(error([&] { model.inferAdmin("request"); }) == "admin_memory_reserve_unavailable" && stub.run == runs, "1 GiB guard runs before inference");
    // Recovery loads a fresh model; a rejected inference does not replay output.
    model.leaveSetup("frozen"); model.enterSetup();
    r.memory.availableKiB = SetupResources::GiBKiB - 1;
    const auto bootstrapRuns = stub.run;
    check(!error([&] { model.useAdminContext("changed", 128); }).empty() && stub.run == bootstrapRuns, "context refresh bootstrap also guarded");
}
void lateCommandAfterDrainFailure(const std::string& root) {
    Rig r;
    RuntimeModel model("fake-runtime", "fake-admin", r.resources, [](const auto&) {});
    RuntimeGenerations runtime(model);
    runtime.apply(admin_test::catalog(admin_test::base(root)));
    const auto original = runtime.current();
    auto admitted = runtime.beginRequest();
    runtime.setAudioDrain([] { throw std::runtime_error("audio_drain_timeout"); });
    check(error([&] { runtime.enterSetup(); }) == "audio_drain_timeout", "in-flight voice drain failed");
    const auto result = admitted->process("Включи свет на кухне", firefly::execution::ExecutionIntent::Production, "voice");
    check(!result.execution && result.guardError == "restart_required", "late admitted voice command cannot actuate after failed Setup drain");
    check(runtime.history().empty() && runtime.current() == original, "late completion preserves configuration and hardware state");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        budgetPolicy(); delayedReclaim(); linuxAccounting(); transitions(argv[1]); failures(argv[1]); guardedInference(); lateCommandAfterDrainFailure(argv[1]); failedDestroyIsTerminal(argv[1]);
        check(stub.activeHandles == 0 && stub.peakHandles == 1, "all fake vendor handles released");
        std::cout << "PASS " << checks << " resource/setup checks (fake RKLLM only)\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
