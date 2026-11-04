# Firefly Voice Assistant

Local Russian voice assistant for Firefly RK3588S (8 GB, Ubuntu 22.04/aarch64). M19 runs wake «Амина» → VAD → Zipformer STT → Runtime Qwen3-4B / deterministic language matching → SafetyGate → configured HTTP device execution. Vue Web Admin provides zones/devices, connections, Review → Apply and local AI Setup with a separate Admin Qwen context.

**Start here:** [production baseline and acceptance limits](docs/production-baseline.md) → [architecture](docs/architecture.md) → [build and isolated tests](docs/build-and-test.md) → [release and recovery](docs/release-procedure.md). Agents must also read [AGENTS.md](AGENTS.md).

**2026-11-01 release-input update:** [Amina/Amila model promotion](docs/amina-amila-release.md) and [accepted F2 pilot handoff for later integration](docs/f2-voice-pilot-handoff.md). These documents describe the new source inputs; the installed baseline below is a dated observation.

**Operational dialogue development:** [grounded voice questions, F2 replies and live settings](docs/operational-voice-dialogue.md). Opt-in source integration; no production activation is implied. [Natural feedback and silent confirmed success](docs/operational-natural-feedback.md); [historical latency measurements](docs/operational-f2-latency.md).

**Voice input development:** [after-beep default and optional direction experiment](docs/after-beep-voice-input.md). Standalone wake → beep → command is the appliance default; automatic inline input is an explicit option.

**Wake filtering experiment:** [STT confirmation before the beep](docs/wake-stt-confirmation.md), disabled by default; includes measured speech misses and trial limits.

**Device telemetry development:** [availability, last-known state and periodic ESP32 reporting](docs/device-telemetry.md). Online status requires independent device data; broker acknowledgements do not establish it.

## Version and scope

As verified read-only on **2026-10-16**, fetched Git master is `c6e68419eea1f47300f7ca8d51b85997344e405c` (PR #27 merged). Installed release is `m19-c7fa1490-20261015T150156Z`, built from exact product commit `c7fa14906f8ae15f4c8fb488e9c9d7628eb8154f`; annotated tag `m19-production-20261015-c7fa149` points to that product commit. Master contains later operator/tests/docs changes. These identities are deliberately distinct and must be rechecked for future work.

M19 is accepted as **PARTIAL_PASS**: real Admin4096 Setup/Apply and instrumented voice ON passed; the owner also accepted ON/OFF. Two bounded Admin cycles passed. Full 4096-token saturation, prolonged soak and a complete machine-instrumented OFF E2E are not established. Historical reports retain their original limits.

HTTP supports fixed numeric-IPv4 endpoints, action bindings and separate GET observations. MQTT is a disabled planned transport, **not implemented**. No MQTT work is included in this baseline cleanup.

## Open the installed Admin from the PC

```sh
ssh -N -o ExitOnForwardFailure=yes -L 127.0.0.1:18080:127.0.0.1:8080 firefly@192.168.68.111
```

Keep the tunnel open and visit `http://127.0.0.1:18080/`. Browsing does not pause voice. Starting **AI Настройка** temporarily transfers resources to Admin Qwen; **Завершить настройку без Apply** restores Runtime without applying the draft. **Сохранить и применить** changes the active configuration. Do not use production as an offline test backend.

## Quick PC verification

Requires Linux, C++17 compiler, CMake ≥3.16, Python 3 and Node ≥22.12.0. Install frontend dependencies from the committed lockfile on the PC (or use a matching offline cache), then:

```sh
cd src/admin/frontend
npm ci
npm run typecheck
npm test
npm run build
cd ../../..
env -u DISPLAY cmake -S . -B build/host \
  -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j2
ctest --test-dir build/host --output-on-failure
```

This builds native PC CPU tests and isolated fake-worker Admin backends; it neither builds a production RKLLM appliance nor tests real speech/NPU. DISPLAY is unset to exclude legacy browser acceptance registration; see the build guide for coverage and safe browser testing.

## Repository map

| Path | Role |
| --- | --- |
| `include/firefly`, `src` | Typed configuration/action/language/safety, inference, voice, execution and Admin code |
| `src/admin/frontend` | Current Vue Admin; lockfile is authoritative; generated dependencies/dist stay ignored |
| `config` | Schema, templates and regression seeds; not the installed ConfigStore |
| `tests` | CPU, stub-runtime, HTTP, frontend, packaging and recovery regressions; hardware entry points are explicitly separate |
| `packaging`, `scripts` | Existing immutable release, supervised rollout/recovery and PC/offboard harnesses |
| `assets/wake`, `runtime/wake` | Frozen Amina artifact and PCM worker |
| `benchmarks`, `experiments` | Historical measurements and referenced regressions, not disposable by name |
| `docs/evidence` | Published historical proof; private operational evidence/backups remain on the PC |
| `third_party` | Vendored JSON header and license; no downloaded models |

No GitHub Actions workflow is present in this baseline. Checks are run explicitly; do not interpret a merged PR as evidence of automatic CI. [Cleanup inventory and verification](docs/repository-baseline-audit.md) records what was retained and why. MQTT should extend the existing Device → Action → Binding → Connection boundary; see the architecture guide before implementation.
