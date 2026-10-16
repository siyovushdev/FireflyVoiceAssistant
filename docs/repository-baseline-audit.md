# M19 repository baseline audit — 2026-10-16

Scope: local branch `chore/m19-production-baseline`, separate `baseline-worktree`, based on fetched master `c6e68419eea1f47300f7ca8d51b85997344e405c`. No publication, PR, merge, tag, rollout, service operation, inference, microphone use or MQTT implementation.

## Findings and cleanup decisions

The 575 tracked files contained no generated build objects, node_modules or unclassified temporary logs. Tracked logs are published evidence. Therefore **no tracked source, test, evidence or Firefly directory was deleted**. Cleanup is documentation consolidation and safer offline test setup, not an arbitrary file-count reduction.

| Area | Finding / decision |
| --- | --- |
| README / AGENTS | README had no current M19 identity; AGENTS still prescribed M1–M4 behavior. Replaced with current entry points; archived old agent instructions, retained detailed CLI reference. |
| Release procedure | Wrong current release and unconditional old/new assets equality. Updated to current identities and explicit reviewed new-assets pin; moved incident narrative to a dated historical page. |
| Architecture | Added current ownership, typed safety/transport boundaries, ConfigStore and Setup flow; marked older design/checkpoint pages historical. |
| CMake | Explicit Linux host offline test mode added. Real assistant/voice stay disabled there and production remains aarch64-only; default configuration cannot bypass the target guard. |
| Recovery tests | Three mocked fuser tests still stat'ed real RK3588 device paths on PCs. Mocked filesystem metadata instead of S_ISCHR; added rejection of a non-character resource. Production recovery code is unchanged. |
| `src/legacy`, `tests/reference` | Referenced by CMake and regressions. Kept as compatibility/safety oracle. |
| `experiments/stt-wake-55` | Standalone evaluation referenced by M5.5 reports; kept with original evidence limits. |
| `benchmarks` | Several targets are built by CMake; others preserve measurements/replay constraints. Kept. |
| `scripts`, `packaging` | Active build/release/recovery/PC harness chain and older reproducible scenarios. Kept. |
| Two identical semantic JSON fixtures | `grounding-semantic.json` and `m19-live-semantic.json` have byte-identical SHA256, but distinct regression/backport and evidence provenance. Kept rather than breaking historical references. |
| Branches | Remote refs audited; only `origin/feature/web-admin-ux-v2-b4` is not an ancestor of current master (old commit `6d6ac8c`). No remote branch deleted or history rewritten; ancestry alone is not proof of dispensability. |
| CI | No GitHub Actions workflow exists; explicit local checks are documented instead of claiming automatic CI. |

The installed product code equals the master product source; later commits between c7 and master are operator/tests/docs changes. This cleanup changes no runtime source, schema, frontend behavior, release controller or installed artifact. The old public build label “Milestone 17” is documented as non-authoritative; it was not patched into a new release.

## Firefly inventory

Read-only sequential low-priority scans found 24 project checkouts (7.836 GiB unique allocated), 6.363 GiB of per-path build directories and 1.255 GiB of per-path frontend dependencies (subsets, not additive guaranteed savings). The old M19 checkout has 33 dirty entries; four later worktrees also contain operator/test overlays. All remain preserved.

Disk availability is approximately 13.082 GiB. Installed shared models occupy 9.089 GiB and active release 108.98 MiB; one active installation is about 9.196 GiB. Ten immutable releases, the 108.99 MiB c7 staging directory, 14 project archives and 135 `/run` recovery/fixture roots were inventoried, not deleted. Active c7, rollback 320 and grounding Amina anchor are protected. Base build Sherpa/wake dependencies and the single-json dist are still referenced: do not bulk-delete old builds.

Exact per-path tables, dirty status, Git refs, hardlink-aware aggregate, archive list and low-priority scan commands are saved privately on the control PC at `evidence/m19-baseline-20261016/DISK-INVENTORY.md`, `firefly-inventory.json`, `extended-inventory.json`. No unknown data outside the isolated worktree was removed.

## Verification

See the final results below; host CPU results are separate from historical native aarch64/NPU qualification. All production inspection is read-only; ConfigStore compatibility uses a protected PC copy. No owner voice or hardware acceptance is repeated. Historical native full packaging/85-suite acceptance remains historical and does not count as a fresh run here.

- Clean native Linux/x86_64 host CPU CMake build: PASS. This is not an aarch64 release build.
- CTest: initial 63/64 PASS; ReleaseRecovery failed because the mock left real device stat calls. After the test-only correction, affected suite passed (35 unittest cases including the new non-character rejection). All 64 registered suites are covered successfully; the 63 unaffected suites were not repeated. GroundingAuthorizationHttp covered its 450-case corpus.
- Frontend: typecheck PASS, 41/41 tests PASS, production dist build and offline asset allowlist PASS.
- Extra isolated M19 operator tests: recovery 16, start 10, PC transport 16, diagnostics/stress harness 19 — all PASS. These do not run real model stress.
- `ConnectivityHttpTests.py` and `UnifiedHttpTests.py`: PASS with a separately compiled PC `unified_http_test_app`, exact preserved SDK header and `FakeRkllmRuntime.cpp`, linked against fresh host libraries. Real loopback POST/GET; private stores/mock, no Firefly requests. Build invocation is saved in private evidence.
- Four host-build rejection checks PASS: default x86 production config, host+assistant, host+voice, host without testing. Existing aarch64 production guard remains in place.
- ConfigStore compatibility: protected PC copy validates schema v2, revision 2 and original content hash `688690e892d2f59413cdc2f7d81f1e129ab9ac0dd247f84b2898d74c3c3cdd18`. No save/restore/Apply.
- Installed package: all 1060 artifact hashes/sizes PASS. Shared models checked by pinned manifest, sizes and before/after metadata; no new multi-GiB model hashing or inference.
- Production before/after: identical PID 1456082, release, binary hash, ConfigStore bytes/ownership/modes/mtimes, model metadata, permanent unit hash, argv and NPU/microphone owners. READY / WAITING_WAKE, NRestarts=0. Retained private recovery archive SHA256 matches `ac8287fd4e39cdfa3b4559984acfc0f47ad5d722ce8831a442f9a7dd3671a63d`; consumed plan remains historical.

Not freshly run: native aarch64 appliance/85-suite build, full ProductionPackaging (native libraries/wake environment), AminaStreamingBackend, four Chromium CTest suites, real systemd integration fixtures, real NPU/voice, full ctx4096 saturation or soak. The host CTest subset includes ReleaseRecovery, DeploymentLauncher, ReleaseRunner, InstalledModelReferences and AdminV2Packaging; it is not full production packaging certification.

Verdict: **READY_WITH_LIMITATIONS** for starting a separately scoped M20 MQTT design/implementation task. Existing HTTP typed device/action/connection flow, SafetyGate, ConfigStore and Review→Apply remain compatible; MQTT requires additive mapping/executor/validation/UI and delivery/observation policy work, not replacement of these boundaries. No MQTT code is added. New release/hardware acceptance still needs its own native gates and authorization. All changes remain local; no push/PR/merge/tag.
