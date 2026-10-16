# Architecture at the M19 baseline

See [production identity/limits](production-baseline.md) and [build/test boundaries](build-and-test.md). This overview describes the current implementation; milestone design documents retain their historical observations.

## Command and voice path

```text
ALSA / reSpeaker → Amina wake + BEEP → Silero VAD → Russian Zipformer STT
  → original text → generation.LanguageIndex / conservative parser / Runtime Qwen
  → typed GenericAction → SafetyGate → config/domain validation
  → BindingResolver → ExecutorRegistry → HttpHardwareExecutor
  → accepted command: Predicted state
  → separate configured GET: validated Confirmed state
```

The original text remains the safety evidence. Unsupported, ambiguous or ungrounded actions stop before transport; model output cannot invent device identities, repair safety violations or choose arbitrary URLs. Runtime context is 1024; compact scalar output is selected using validated configuration. Legacy `D`, `L|…`, `U` survive only at compatibility/debug interfaces. Keep `src/legacy` and `tests/reference`: regression suites explicitly use them to protect prior semantics.

`RuntimeGeneration` owns one immutable validated Catalog, DomainView, LanguageIndex and executor view. `ConfigStore` provides stable identities, revision/hash CAS, backup/LKG and atomic publication. The current language compiler ID is `language_atoms_v2_location_grammar`; older language-index documents describe earlier algorithm revisions.

## Admin lifecycle and publication

Vue assets are built offline, allowlisted/hash-verified and embedded in the C++ HTTP server. Node and frontend source/dependencies are not runtime services. Same-origin mutation guards apply; loopback SSH tunnelling preserves the existing boundary.

Setup closes command admission, drains voice, releases Runtime RKLLM and command STT/VAD, then admits Admin4096 under the 6 GiB / 1 GiB resource policies. Exactly one in-process RKLLM owner is allowed. This does not protect against an unrelated second process; operators must preserve exclusive NPU/microphone ownership. Invalid teardown/admission fails closed. Admin output is strict validated proposals into a private draft; it does not execute hardware. Review → explicit Apply commits the validated catalog and rebuilds Runtime; Cancel restores Runtime without publishing the draft. Command STT/VAD/wake resume only after resource restoration.

Sources: `src/admin/RuntimeGeneration.cpp`, `RuntimeWorker.cpp`, `SetupSession.h`, `src/SetupResources.cpp`, `src/VoiceRecovery.cpp`. Detailed lifecycle: [Resource-Aware Setup](m19-resource-aware-setup.md).

## Device connectivity and the M20 boundary

A logical Device exposes typed Actions and state. A Binding maps an existing `(target_id, action_id)` to a transport; a reusable Connection owns the endpoint. ObservationBinding is separate. SafetyGate and semantic domain validation precede BindingResolver; transports receive typed actions and validated mappings, never free-form voice instructions.

HTTP currently supports numeric IPv4 `http://`, fixed paths and scalar JSON mappings; no TLS, DNS, credentials store, redirect following or automatic command retries. Legacy full-URL bindings and Connection-based bindings both remain supported. Config v2 extension arrays preserve canonical omission when empty. See [connectivity contract](device-connectivity-binding-admin.md).

MQTT/TCP/UDP/Serial/CAN names are planned disabled configuration choices. They do not imply working executors. MQTT requires a reviewed executor, mapping validation and explicit publish/observe/error/reconnect semantics; broker authentication and delivery policy are unresolved design work. `HardwareExecutor` / `ExecutorRegistry` provide the extension seam. Logical devices/actions, SafetyGate, ConfigStore CAS and Review→Apply need no architectural replacement, but MQTT-specific fields/UI/validation/tests will need additive changes. Preserve existing HTTP canonical bytes, stable IDs and behavior. Do not enable MQTT by merely removing the disabled guard or treat a publish acknowledgement as observed physical state.

## Build, packaging and recovery links

CMake builds core/voice orchestration/ConfigStore/Admin libraries and separate real RKLLM/ALSA targets. Host offline mode excludes real RKLLM/voice targets. Python packaging validates native ELF/dependencies, frozen model references and assets. `stage-release.py` creates an immutable package; installed shared models are referenced without a second multi-GiB copy.

`release-runner.py` prepares and pins a clean candidate, backup, gates and plan. `production-rollout.py` performs the authorized transition; `deployment-launcher.py` submits independently of SSH; `packaging/firefly_recovery.py` restores the pinned old release under systemd supervision. Failed/unknown resource release blocks switching. Tests and file-only fixtures are not evidence of real NPU acceptance.

There is no configured GitHub Actions workflow. CTest/frontend checks are explicit, and native full packaging/systemd/hardware checks have distinct prerequisites and authorization. See [release procedure](release-procedure.md).
