#include "Support.h"
#include "firefly/voice/VoiceRecovery.h"
#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using firefly::voice::VoiceRecovery;
using Clock = VoiceRecovery::Clock;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Simulated {
    Clock::time_point now{};
    bool stopping = false, blocked = false;
    int attempts = 0, failures = 0;
    std::vector<std::string> events;
    std::vector<Clock::time_point> starts;
    std::function<void()> onWait;
    VoiceRecovery recovery{{[&] { return now; }, [&](auto delay) {
        check(delay > 0ms && delay <= 50ms, "bounded nonzero polling");
        recovery.drain(); // No active audio during backoff or setup wait.
        now += delay;
        if (onWait) onWait();
    }}};
    VoiceRecovery::Hooks hooks{
        [&] { return stopping; }, [&] { return blocked; },
        [&] { check(!blocked, "no opening during setup"); starts.push_back(now); ++attempts; },
        {}, [&](const auto&) { ++failures; },
        [&](const auto& message) { events.push_back(message); }
    };
};
void retryPolicy() {
    Simulated s;
    s.hooks.attempt = [&](const auto& ready) {
        if (s.attempts == 8) { s.stopping = true; return; }
        // A newly opened but immediately failing path must NOT reset backoff.
        if (s.attempts >= 2) ready();
        if (s.attempts == 6) s.now += 10s; // Stable session resets the NEXT delay.
        throw std::runtime_error("capture read failure");
    };
    s.recovery.run(s.hooks);
    const std::vector<Clock::duration> expected{1s, 2s, 4s, 8s, 10s, 11s, 2s};
    check(s.starts.size() == 8 && s.failures == 7, "failure must retry instead of terminate");
    for (size_t i = 1; i < s.starts.size(); ++i)
        check(s.starts[i] - s.starts[i - 1] == expected[i - 1], "progression/cap/stable reset");
    check(std::count(s.events.begin(), s.events.end(), "scheduled delay_ms=10000") == 1, "capped retry scheduled");
    check(std::count(s.events.begin(), s.events.end(), "scheduled delay_ms=1000") == 2, "healthy reset");
    s.recovery.drain();
}
void stopAndBlock() {
    Simulated s;
    s.hooks.attempt = [&](const auto&) { throw std::runtime_error("open failed"); };
    s.onWait = [&] { s.stopping = true; };
    s.recovery.run(s.hooks);
    check(s.attempts == 1 && s.now.time_since_epoch() == 50ms, "shutdown interrupts first retry slice");

    Simulated b;
    b.blocked = true;
    b.onWait = [&] { if (b.now.time_since_epoch() >= 500ms) b.blocked = false; };
    b.hooks.attempt = [&](const auto&) {
        if (b.attempts == 1) { b.blocked = true; return; } // Intentional drain.
        b.stopping = true;
    };
    b.recovery.run(b.hooks);
    check(b.starts.front().time_since_epoch() == 500ms, "waits for setup release");
    check(b.attempts == 2 && b.failures == 0 && b.events.empty(), "setup drain is not a failure/retry");

    Simulated during;
    during.hooks.attempt = [&](const auto&) {
        if (during.attempts == 1) throw std::runtime_error("absent");
        during.stopping = true;
    };
    during.onWait = [&] {
        during.blocked = during.now.time_since_epoch() < 1500ms;
    };
    during.recovery.run(during.hooks);
    check(during.starts.back().time_since_epoch() == 1500ms, "expired retry waits for setup");
}
void cappedStopAndUnexpectedReturn() {
    Simulated s;
    s.hooks.attempt = [&](const auto&) {}; // Unexpected normal return is not a hot loop.
    s.onWait = [&] { if (s.attempts == 7) s.stopping = true; };
    s.recovery.run(s.hooks);
    check(s.failures == 7, "unexpected end retries safely");
    check(s.starts.back().time_since_epoch() == 35s, "repeated capped backoff");
    check(s.now - s.starts.back() == 50ms, "shutdown interrupts maximum backoff");
}
void actualSetupDrain(const std::string& root) {
    struct Model : admin_test::Model {
        std::atomic<int>& owners;
        explicit Model(std::atomic<int>& count) : owners(count) {}
        void enterSetup() override { check(owners == 0, "model switching before audio teardown"); }
        void leaveSetup(const std::string& prompt) override { useContext(prompt); }
    };
    std::atomic<int> owners{0}, attempts{0}, failures{0};
    std::atomic<bool> stopping{false}, releaseCleanup{false}, cleanupEntered{false};
    Model model(owners);
    firefly::admin::RuntimeGenerations runtime(model);
    runtime.apply(admin_test::catalog(admin_test::base(root)));
    VoiceRecovery recovery;
    runtime.setAudioDrain([&] { recovery.drain(); });
    auto services = std::async(std::launch::async, [&] {
        recovery.run({[&] { return stopping.load(); }, [&] { return runtime.setupBlocked(); }, [] {},
            [&](const auto& ready) {
                check(++owners == 1, "concurrent audio owners");
                ++attempts;
                ready();
                while (!stopping && !runtime.setupBlocked()) std::this_thread::yield();
                cleanupEntered = true;
                while (!stopping && !releaseCleanup) std::this_thread::yield();
                --owners;
            }, [&](const auto&) { ++failures; }, [](const auto&) {}});
    });
    auto waitUntil = [](const auto& predicate) {
        const auto deadline = Clock::now() + 2s;
        while (!predicate() && Clock::now() < deadline) std::this_thread::yield();
        check(predicate(), "thread rendezvous timeout");
    };
    // Always release/join on assertion failure as well.
    struct Stop { std::atomic<bool>& flag; ~Stop() { flag = true; } } stop{stopping};
    waitUntil([&] { return attempts == 1; });
    auto setup = std::async(std::launch::async, [&] { runtime.enterSetup(); });
    // Avoid leaving the setup future blocked if a subsequent assertion fails.
    struct Release { std::atomic<bool>& flag; ~Release() { flag = true; } } release{releaseCleanup};
    waitUntil([&] { return cleanupEntered.load(); });
    check(setup.wait_for(0ms) == std::future_status::timeout, "drain waits until teardown completed");
    releaseCleanup = true;
    check(setup.wait_for(2s) == std::future_status::ready, "setup drain completes");
    setup.get();
    check(owners == 0 && attempts == 1, "no audio while setup active");
    recovery.drain();
    runtime.prepareRestore();
    check(attempts == 1, "no reopen during model restore");
    runtime.finishRestore();
    waitUntil([&] { return attempts == 2; });
    stopping = true;
    check(services.wait_for(2s) == std::future_status::ready, "service joins cleanly");
    services.get();
    check(owners == 0 && failures == 0, "intentional setup was not infrastructure failure");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        retryPolicy(); stopAndBlock(); cappedStopAndUnexpectedReturn(); actualSetupDrain(argv[1]);
        std::cout << "PASS voice recovery policy, shutdown, setup/drain ownership\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
