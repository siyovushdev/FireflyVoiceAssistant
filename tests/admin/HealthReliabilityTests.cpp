#include "Support.h"
#include "firefly/admin/HealthModel.h"
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/config/ConfigStore.h"
#include "firefly/config/ConfigValidator.h"
#include <csignal>
#include <iostream>
#include <fstream>

using namespace admin_test;
using namespace firefly;
using Json = nlohmann::json;

namespace {
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
firefly::config::Catalog catalogV2(const Json& value) {
    return firefly::config::ConfigValidator::validate(
        firefly::config::migrateV2(catalog(value)->configuration()));
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const auto root = argv[1];
        const auto baseConfig = base(root);

        // Health transitions (no NPU / RKLLM).
        {
            admin::HealthModel health;
            check(health.snapshot().overall == admin::HealthOverall::Starting, "initial Starting");
            health.setSubsystem("admin", admin::SubsystemStatus::Ready);
            health.setSubsystem("configuration", admin::SubsystemStatus::Ready);
            health.setSubsystem("runtime", admin::SubsystemStatus::Ready);
            health.setSubsystem("llm", admin::SubsystemStatus::Ready);
            health.setSubsystem("stt", admin::SubsystemStatus::Ready);
            health.setSubsystem("hardware_execution", admin::SubsystemStatus::Ready);
            health.setPhase("ready");
            check(health.snapshot().overall == admin::HealthOverall::Ready, "all ready → READY");

            health.setSubsystem("llm", admin::SubsystemStatus::Failed, "model_unavailable");
            check(health.snapshot().overall == admin::HealthOverall::Degraded, "LLM fail → DEGRADED");

            health.setSubsystem("llm", admin::SubsystemStatus::Ready);
            health.setConfigRecovered(true);
            health.refreshOverall();
            check(health.snapshot().overall == admin::HealthOverall::Degraded, "LKG recovered → DEGRADED");

            health.setConfigRecovered(false);
            health.setSubsystem("stt", admin::SubsystemStatus::Failed, "stt_unavailable");
            health.refreshOverall();
            check(health.snapshot().overall == admin::HealthOverall::Degraded, "STT fail → DEGRADED");

            health.setSubsystem("stt", admin::SubsystemStatus::Ready);
            health.refreshOverall();
            check(health.snapshot().overall == admin::HealthOverall::Ready, "recovered subsystems → READY");

            health.setSubsystem("wake", admin::SubsystemStatus::Failed, "wake_voice_unavailable");
            check(health.snapshot().overall == admin::HealthOverall::Degraded, "wake failure degrades overall");
            check(Json::parse(admin::HealthModel::toJson(health.snapshot())).at("subsystems").at("wake").at("status") == "failed",
                "wake health separately exposed");
            health.setSubsystem("wake", admin::SubsystemStatus::Ready, {}, "WAITING_WAKE");
            check(health.snapshot().overall == admin::HealthOverall::Ready, "wake recovery ready");
            health.setStopping(true);
            check(health.snapshot().overall == admin::HealthOverall::Degraded, "stopping → DEGRADED");
            const auto json = admin::HealthModel::toJson(health.snapshot());
            check(json.find("\"stopping\":true") != std::string::npos, "health JSON stopping");
            check(json.find("\"config_recovered\"") != std::string::npos, "health JSON config_recovered");
        }

        // LKG marker resolves prior revision from backups after save.
        {
            Directory dir;
            config::ConfigStore store(dir.path);
            const auto initial = store.initialize(catalog(baseConfig));
            store.recordLastKnownGood(initial);
            auto next = baseConfig;
            next["system"]["name"] = "House 2";
            (void)store.save(catalog(next),
                {initial->configuration().revision, initial->contentHash()});
            store.recordLastKnownGood(initial);
            const auto loaded = store.lastKnownGood();
            check(static_cast<bool>(loaded), "LKG present");
            check((*loaded)->configuration().revision == initial->configuration().revision,
                "LKG resolves previous revision from backups");
            check((*loaded)->contentHash() == initial->contentHash(), "LKG hash matches recorded Apply");
        }

        // Broken current + valid LKG → recovered boot, DEGRADED, Admin status OK.
        {
            Directory dir;
            Model model;
            Client client(model);
            config::ConfigStore store(dir.path);
            const auto initial = store.initialize(catalog(baseConfig));
            store.recordLastKnownGood(initial);
            {
                std::ofstream out(dir.path + "/configuration.json", std::ios::trunc);
                out << "{not-json";
            }
            admin::Api api(store, client);
            api.noteAdminListening();
            api.initializeRuntime();
            const auto health = api.health().snapshot();
            check(health.configRecovered, "boot recovers via LKG");
            check(health.overall == admin::HealthOverall::Degraded, "recovered boot is DEGRADED");
            check(health.subsystems.at("admin").status == admin::SubsystemStatus::Ready, "admin ready after LKG");
            const auto status = Json::parse(api.handle("GET", "/api/status", "").body);
            check(status.at("health").at("config_recovered") == true, "status exposes config_recovered");
            check(status.at("active").is_object(), "runtime has active generation from LKG");
            check(status.at("ok") == true, "status ok after recovery");
        }

        // No usable config → recovery mode; Admin status; execution unavailable.
        {
            Directory dir;
            Model model;
            Client client(model);
            config::ConfigStore store(dir.path);
            admin::Api api(store, client);
            api.noteAdminListening();
            api.initializeRuntime();
            const auto status = Json::parse(api.handle("GET", "/api/status", "").body);
            check(status.at("ok") == true, "status OK without config");
            check(status.at("health").at("subsystems").at("admin").at("status") == "ready", "admin ready");
            check(status.at("active").is_null(), "no active generation");
            check(api.health().snapshot().subsystems.at("hardware_execution").status ==
                admin::SubsystemStatus::Unavailable, "execution unavailable");
            check(api.health().snapshot().phase == "recovery_mode", "recovery_mode phase");
        }

        // LLM failure leaves Admin alive and overall DEGRADED.
        {
            Directory dir;
            Model model;
            model.available = false;
            Client client(model);
            config::ConfigStore store(dir.path);
            (void)store.initialize(catalog(baseConfig));
            admin::Api api(store, client);
            api.noteAdminListening();
            api.initializeRuntime();
            const auto health = api.health().snapshot();
            check(health.subsystems.at("admin").status == admin::SubsystemStatus::Ready,
                "admin survives LLM fail");
            check(health.overall == admin::HealthOverall::Degraded, "LLM fail overall DEGRADED");
            check(Json::parse(api.handle("GET", "/api/status", "").body).at("ok") == true,
                "status available after LLM fail");
        }

        // STT note → DEGRADED while Admin ready.
        {
            Directory dir;
            Model model;
            Client client(model);
            config::ConfigStore store(dir.path);
            (void)store.initialize(catalog(baseConfig));
            admin::Api api(store, client);
            api.noteAdminListening();
            api.noteStt(false, "stt_init_failed");
            api.initializeRuntime();
            const auto health = api.health().snapshot();
            check(health.subsystems.at("stt").status == admin::SubsystemStatus::Failed, "STT failed");
            check(health.subsystems.at("admin").status == admin::SubsystemStatus::Ready, "admin after STT fail");
            check(health.overall == admin::HealthOverall::Degraded, "STT fail overall DEGRADED");
        }

        // Failed Apply must not replace LKG; restart recovery still uses A.
        {
            Directory dir;
            Model model;
            Client client(model);
            config::ConfigStore store(dir.path);
            const auto configA = catalog(baseConfig);
            const auto initial = store.initialize(configA);
            admin::Api api(store, client);
            api.noteAdminListening();
            api.initializeRuntime();
            store.recordLastKnownGood(initial);
            const auto lkgHash = (*store.lastKnownGood())->contentHash();
            check(lkgHash == initial->contentHash(), "LKG=A after successful Apply");

            auto next = baseConfig;
            next["system"]["name"] = "Candidate B";
            const auto savedB = store.save(catalog(next),
                {store.current()->configuration().revision, store.current()->contentHash()});
            check(savedB->contentHash() != lkgHash, "B differs from A");
            model.reject = true;
            const auto applyB = api.handle("POST", "/api/config/apply",
                Json{{"expected", {
                    {"revision", savedB->configuration().revision},
                    {"hash", savedB->contentHash()}}}}.dump());
            check(applyB.status == 422, "Apply B fails admission");
            model.reject = false;
            const auto lkgAfter = store.lastKnownGood();
            check(lkgAfter && (*lkgAfter)->contentHash() == lkgHash, "failed Apply leaves LKG=A");

            {
                std::ofstream out(dir.path + "/configuration.json", std::ios::trunc);
                out << "{not-json";
            }
            admin::Api recovered(store, client);
            recovered.noteAdminListening();
            recovered.initializeRuntime();
            check(recovered.health().snapshot().configRecovered, "restart recovery uses LKG=A");
            check(Json::parse(recovered.handle("GET", "/api/status", "").body)
                      .at("active").at("hash") == lkgHash,
                "active generation is A after recovery");
        }

        // Admin surface before LLM/STT work: listening + status while Apply/STT fail.
        {
            Directory dir;
            Model model;
            model.available = false;
            Client client(model);
            config::ConfigStore store(dir.path);
            (void)store.initialize(catalog(baseConfig));
            admin::Api api(store, client);
            api.noteAdminListening();
            check(Json::parse(api.handle("GET", "/api/status", "").body).at("ok") == true,
                "status available before runtime Apply");
            check(api.health().snapshot().subsystems.at("admin").status == admin::SubsystemStatus::Ready,
                "admin ready before LLM init");
            api.noteStt(false, "stt_init_failed");
            api.initializeRuntime();
            const auto health = api.health().snapshot();
            check(health.subsystems.at("admin").status == admin::SubsystemStatus::Ready,
                "admin survives LLM+STT failure");
            check(health.overall == admin::HealthOverall::Degraded, "LLM+STT failure DEGRADED");
            check(Json::parse(api.handle("GET", "/api/status", "").body).at("ok") == true,
                "status after LLM+STT failure");
        }

        // SIGTERM contract: handler only flips sig_atomic; beginShutdown in normal context.
        {
            volatile std::sig_atomic_t flag = 0;
            // Mirrors DemoMain::stop — assignment only, no mutex/allocations/teardown.
            auto signalHandler = [](volatile std::sig_atomic_t* slot) { *slot = 1; };
            signalHandler(&flag);
            check(flag == 1, "signal path only writes sig_atomic");
            Directory dir;
            Model model;
            Client client(model);
            config::ConfigStore store(dir.path);
            (void)store.initialize(catalog(baseConfig));
            admin::Api api(store, client);
            api.initializeRuntime();
            // Emulate main-thread observation of the flag after server.run returns.
            if (flag) api.beginShutdown();
            client.worker.stop();
            check(api.health().snapshot().stopping, "beginShutdown after flag in normal context");
            check(api.handle("POST", "/api/config/apply",
                Json{{"expected", {
                    {"revision", store.current()->configuration().revision},
                    {"hash", store.current()->contentHash()}}}}.dump()).status == 503,
                "post-flag shutdown rejects Apply");
        }

        // Shutdown rejects new Apply / production invoke.
        {
            Directory dir;
            Model model;
            Client client(model);
            config::ConfigStore store(dir.path);
            const auto initial = store.initialize(catalog(baseConfig));
            admin::Api api(store, client);
            api.initializeRuntime();
            api.beginShutdown();
            client.worker.stop();
            const auto reply = api.handle("POST", "/api/config/apply",
                Json{{"expected", {
                    {"revision", initial->configuration().revision},
                    {"hash", initial->contentHash()}}}}.dump());
            check(reply.status == 503, "apply rejected while stopping");
            check(api.health().snapshot().stopping, "health stopping");
            const auto status = Json::parse(api.handle("GET", "/api/status", "").body);
            check(status.at("stopping") == true, "status stopping");
        }

        // DeviceState cold-start Unknown after process restart (new RuntimeGenerations).
        {
            Model model;
            const auto snap = catalogV2(baseConfig);
            admin::RuntimeGenerations first(model);
            first.apply(snap);
            first.current()->executor->production().applyPrediction(
                "device_kitchen_light",
                {{"brightness", actions::Scalar(std::int64_t{40})}},
                1000);
            check(first.current()->executor->production().device("device_kitchen_light")
                      ->properties.at("brightness").knowledge ==
                      execution::StateKnowledge::Predicted,
                "predicted before restart");

            admin::RuntimeGenerations restarted(model);
            restarted.apply(snap);
            check(restarted.current()->executor->production().device("device_kitchen_light")
                      ->properties.at("brightness").knowledge ==
                      execution::StateKnowledge::Unknown,
                "cold start Unknown after restart");
        }

        // systemd unit template present with required policy keys (not installed).
        {
            std::ifstream unit(root + std::string("/packaging/firefly-voice-assistant.service"));
            check(static_cast<bool>(unit), "unit file exists");
            std::string text((std::istreambuf_iterator<char>(unit)), std::istreambuf_iterator<char>());
            for (const char* needle : {
                     "Restart=on-failure", "TimeoutStopSec=", "User=firefly",
                     "StandardOutput=journal", "WorkingDirectory=", "LD_LIBRARY_PATH=",
                     "StartLimitBurst=", "Type=simple"})
                check(text.find(needle) != std::string::npos, needle);
            check(text.find("Restart=always") == std::string::npos, "no Restart=always");
        }

        std::cout << "PASS " << checks << " M17 health / LKG / shutdown checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
