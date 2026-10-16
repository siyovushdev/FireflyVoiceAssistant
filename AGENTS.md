# Repository guidelines — M19 baseline

Read [README.md](README.md) first, then [the dated production baseline](docs/production-baseline.md), [architecture](docs/architecture.md), and [build/test guide](docs/build-and-test.md). These are the current entry points. Earlier milestone reports and [archived M1–M4 instructions](docs/history/agent-instructions-m1-m4.md) preserve historical evidence; they do not define today's scope.

## Target and boundaries

Production is Linux/aarch64 on Firefly RK3588S, Ubuntu 22.04, 8 GB RAM. Keep RKLLM 1.3.0, Qwen3-4B W8A8, Runtime context 1024, separate Admin context 4096, and the 6 GiB admission / 1 GiB reserve policies. Do not change models, quantization, swap or safety thresholds as incidental cleanup. MQTT is not implemented; do not start M20 without an explicit task.

Codex and Chromium run on the Ubuntu PC. Control Firefly over SSH. Inspect production read-only unless the owner explicitly authorizes a hardware window or activation. Never launch a competing NPU/microphone owner. Do not stop/restart production, switch release pointers, mutate ConfigStore or change the permanent unit during ordinary development. SSH credentials, tokens, private ConfigStore backups, raw user audio and model binaries must not be committed. Preserve the already tracked frozen small Amina release artifact.

`~/demo_Linux_aarch64` is read-only. Preserve active/rollback releases, the original grounding release used as the Amina anchor, shared models, evidence, dirty trees and unrelated processes. Use a separate branch/worktree from fetched master; never reset or clean someone else's checkout. Publication, merge, tag and rollout require authorization in the task. Historical one-shot permissions and recovery plans are not reusable.

## Current semantic contracts

Original text → LanguageIndex / conservative parser / scoped Runtime Qwen → typed GenericAction → SafetyGate → domain validation → BindingResolver → registered executor. Keep original-input grounding; unsupported or ambiguous input must never execute. SafetyGate may preserve or reject, never repair or invent. Keep explicit entrance-door opening and contradictory light-state protections; do not correct STT transcripts to force acceptance.

Runtime uses the config-derived compact scalar protocol (`[integer]`, `[]`, `null`); strict structured JSON remains for applicable schemas and Admin operations. `D`, `L|…`, `U` are legacy CLI/debug adapters, not the current internal action model. Do not restore M1 hardcoded rooms or the old prompt as production behavior. ConfigStore owns stable IDs, typed actions, CAS revision/hash, backups/LKG and atomic RuntimeGeneration publication. Preserve legacy HTTP URL bindings and reusable Connection bindings. Predicted command state and independently observed Confirmed state are distinct.

AI Setup owns a private draft. Proposal/acceptance does not execute devices; Review → explicit Apply owns publication. Resource-Aware lifecycle drains voice and releases Runtime before Admin load; failed teardown/admission fails closed. Never substitute scripted inference when claiming real Qwen/voice acceptance.

## Implementation and verification

C++17, CMake, RAII, four-space indentation; headers in `include/firefly`, implementations in `src`, regressions in `tests`. Keep changes focused and public compatibility intact. No unrelated refactoring or new deployment framework.

Use the documented explicit host offline mode for PC CPU tests; never label it an aarch64 production build. Real assistant/voice builds remain aarch64-only. Frontend uses the pinned lockfile and local embedded assets, Node at build time only. Inspect a test's resource effects before running it; model-backed replay, hardware harnesses and systemd coordinator fixtures are not ordinary PC unit tests.

Run applicable CTest, frontend typecheck/tests/build and offline packaging/recovery checks. Record exact exclusions and failures. Do not lower guards to obtain PASS or claim tests that did not run. M19 remains PARTIAL_PASS: bounded two-cycle Admin success is not full 4096-token saturation or a long soak. Owner ON/OFF acceptance is distinct from machine-instrumented OFF coverage.

Use [release-procedure.md](docs/release-procedure.md) for authorized releases. Pin product commit, build identity, package/assets hashes and independent recovery; never reuse a consumed plan or activate an unreviewed build. Report changes, tests, limitations and production preservation, and stop at the task boundary.
