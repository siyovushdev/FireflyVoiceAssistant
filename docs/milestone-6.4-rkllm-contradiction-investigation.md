# Milestone 6.4 — contradiction02 investigation and open safety issue

Date: 2026-09-15. Baseline: `fb8e2ee394602e5808e0cd2e6386ad18d77c0e64`.

**Disposition:** the user explicitly accepted the Milestone 6.4 room/location
cutover after reviewing the controlled baseline/WIP evidence. Milestone 6.4
causality was not established: the clean pre-cutover baseline reproduces the same
false accept. The vendor-internal RKLLM cause remains unlocalized.

**Open issue: `contradiction02` — pre-existing RKLLM output variability and a
deterministic safety gap.** Expected production output remains `U`. Acceptance of
the room cutover does not accept this false action or weaken the safety contract.
This issue must be addressed before further configuration-consumer migration.
No safety fix, DEV change, model/prompt/runtime tuning or acceptance-policy
workaround was applied in the investigation or checkpoint.

## Case and controlled setup

```text
id: contradiction02
category: contradiction
original: На кухне выключи свет на 50 процентов
expected: U
observed false accept: L|K|50
```

The input is exactly 66 UTF-8 bytes, at position 42 in the frozen 48-case DEV set.
Its SHA256 is
`107ef6a194e031b8b35a80e3a1572c35f40bcdf1953fad0ad5fc4fa91e405027`.

A was a clean detached worktree at the baseline checkpoint; B was the unchanged
Milestone 6.4 WIP. Both received separate clean full builds using GCC 11.4.0 and
CMake 3.22.1 with matching options. All NPU runs were sequential on the same
RK3588S device, from `/home/firefly/FireflyVoiceAssistantLinux`, with the same
saved environment, model, prompt, DEV bytes and runtime libraries. Each process
initialized once, bootstrapped with an empty user input, used the frozen checked
KV-clear lifecycle, and destroyed its model. No performance settings were changed:
observed NPU 1 GHz, DMC 2.112 GHz, little/big CPUs 1.8/2.352 GHz.

## Controlled results

The initial observations were WIP legacy 47/48 twice, an earlier baseline binary
48/48, and integrated WIP 48/48. The subsequent fixed plans retained every result
across 32 NPU processes; they did not retry until a desired pass.

| Experiment | A: baseline | B: WIP |
|---|---|---|
| Normal-order legacy, absolute DEV path, two runs each | 48/48 twice | 48/48 twice |
| Normal-order legacy, relative DEV path, two runs each | 48/48 twice | 48/48 twice |
| Isolated case, three fresh processes each | `L\|K\|50`, `U`, `U` | `U`, `U`, `L\|K\|50` |
| Case first in two fresh full-suite processes each | 48/48 twice | 47/48, then 48/48 |
| API-traced normal-order legacy | 48/48 | 48/48 |
| API-traced isolated case | `U` | `U` |
| API-traced integrated assistant | 48/48, raw `U` | **47/48**, raw `L\|K\|50` |
| API plus gate/validator traced legacy | 48/48 | 48/48 |
| API plus gate/validator traced integrated assistant | 48 matching outputs; exit-code note below | 48/48 |
| Direct unchanged RKLLM wrapper, two processes × eight identical calls each | 16 raw `U` responses | 16 raw `U` responses |

All failures were false accepts on this case, with no false rejects or wrong
non-U actions. Full suites retained 17 fast paths and 31 LLM fallbacks. Integrated
runs had zero room-shadow mismatches, including the failing run. The direct
wrapper control excluded parser/configuration code and did not reproduce the
variation; it does not explain or negate the baseline harness failures.

A particular preceding DEV command is not required: the isolated baseline and
WIP runs each vary under identical recorded commands. Request order may still
affect likelihood; these small samples do not establish rates or a vendor mechanism.
In one diagnostic stage run, Python failed decoding deliberately invalid startup
validator probe bytes after all 48 outputs and RKLLM destruction. Raw logs were
retained, but the child exit code was not saved. That run was not repeated.
Observers can affect timing/layout, so traced runs are identified separately.

## Exact divergence and downstream handling

In the traced integrated A/B pair, initialization fields (including reserved
bytes), chat templates, all 32 model-call inputs including bootstrap, and all 32
cache-clear calls matched. Every observed API return code was zero. The first
different raw response was call 26: the answer to DEV case 42. All 25 preceding
raw responses matched; both sides reported 25 prefill tokens for this input.

- Baseline callback: token 52, byte `55` (`U`), followed by empty-text EOS.
- WIP callbacks: tokens 43, 91, 42, 91, 20, 15; bytes `4c 7c 4b 7c 35 30`
  (`L|K|50`), followed by the same empty-text EOS token 151645.

Thus RKLLM itself supplies different answers before trimming, candidate handling,
SafetyGate or strict validation. Both parsers leave the command unresolved and
send its original text unchanged to fallback. Both the legacy harness and the
integrated assistant score post-gate, post-validator actions.

For this input, `U` remains `U`; `L|K|50` normalizes unchanged, **SafetyGate preserves
it, and ActionValidator accepts kitchen brightness 50**. The unchanged gate checks
an explicit room and rejects zero brightness with an on-word; it has no general
off/nonzero contradiction rejection. This gap predates 6.4 and was noted in the
[6.1 design](milestone-6.1-dynamic-configuration-design.md). A recorded-output replay
confirmed acceptance through both legacy and injected CommandEngine paths. The
integrated assistant's earlier correct result came from raw `U`, not an extra gate.

## Artifact and runtime identity

Both sides used these exact shared files under `/home/firefly/demo_Linux_aarch64/`:
model `Qwen3-4B_W8A8_RK3588_3core.rkllm`, prompt
`prompt_compact_v5_short_system.txt`, header `rkllm.h`, and runtime
`lib/librkllmrt.so`. RKLLM runtime/toolkit was 1.3.0; RKNPU driver 0.9.8,
kernel 6.1.118. Actual mapped shared-library paths and hashes matched.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| DEV, both worktrees | 3851 | `b9a882c4ea6f7c0b956ca47ad4d2ff320fbdf4eabecb5a378449e34efd23dff0` |
| Prompt | 3138 | `d1c977f28ec4f8e08e30ee362d108fed1482a690667d6af88776ce7c8442d02f` |
| Model | 4840064228 | `d806976a3d77f2252a6abe40a5fd7d9b1006a90f671992923ac2c1542456ba8b` |
| RKLLM library | 7617472 | `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6` |
| RKLLM header | 22925 | `80596a578f7f8e70df6eda1c2cbead3bfced14623a190258f2bd009a3d1f72cf` |

## Build/link and memory audit

The legacy harness link command, dependency order, compiler flags/definitions,
normalized include paths and library resolution matched. It links only
`firefly_rkllm`, `firefly_core` and the same external RKLLM runtime; no configuration,
RuntimeDomainView or nlohmann objects were accidentally linked. Only
`src/runtime/CMakeLists.txt` changed in 6.4, adding tests; root and configuration
CMake files did not change.

The RKLLM wrapper, InputPolicy, Text, Action, SafetyGate and validator objects were
byte-identical. Parser/CommandEngine objects changed for the authorized cutover;
the unchanged harness source recompiles against the changed CommandEngine layout.
Fresh binary SHA256 values exactly match the previously tested binaries:

- A: `eaccc3596f83160cbeef7ce1a9fc4c2e3df0ad44a6de6f75208550bf5facf810`.
- B: `b3a36edf0befe0206129b9ec4bff3bb3e93cb44a3db88ac7f7d87ed0f056b9b0`.

Clean full builds passed. Baseline CTest passed **16/16**, WIP **19/19**, and a
separate ASan/UBSan core build passed **6/6** suites with no relevant failure.
Inspection found no application lifetime error, uninitialized state or ignored
setup/cache-clear failure. Sanitizers did not instrument the vendor runtime and
do not establish vendor memory safety. Its internal cause remains unlocalized.

## Acceptance boundary and retained evidence

The deterministic/config-backed room cutover passed verification. A historical
48/48 RKLLM result cannot be treated as deterministic evidence for this specific
case when the clean baseline also varies. The user's explicit acceptance applies
to Milestone 6.4 only; the open false-accept issue above remains a prerequisite
before further consumer migration. Expected DEV output and the production safety
contract are unchanged. No tuning, safety repair or policy workaround was applied.

This tracked report preserves the essential evidence independently of build
directories. Detailed raw logs, per-run commands/environment metadata, observer
sources and hash manifests remain locally in
`build/contradiction02-investigation/`, including `INVESTIGATION.md`,
`all-results.json`, `artifacts.json`, `critical-trace-comparison.json`,
`build-comparison.json` and the numbered logs. Large generated artifacts and the
private environment record are excluded from git. The accepted implementation
and checkpoint verification are recorded in the
[room cutover report](milestone-6.4-parser-room-cutover.md).
