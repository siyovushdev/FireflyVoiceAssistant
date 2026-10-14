# M19: controlled acceptance from the PC

## Current result — 2026-10-14

M19 has a bounded **PARTIAL_PASS**. Real Admin Qwen3-4B ctx4096 initialized and
completed five inference calls. Its original proposal created Кабинет / Лампа,
«лампа», «лампу», «в кабинете», boolean power and ON/OFF actions. Review and
exactly one Apply succeeded in a private test ConfigStore. Runtime Qwen and the
voice components returned to READY / WAITING_WAKE.

The instrumented human ON command passed through the real microphone, wake,
VAD, STT, Runtime and SafetyGate to one private HTTP POST and a separate GET.
The owner separately reported successful ON and OFF. Instrumented OFF in the
private candidate window remains NOT_TESTED: an extra harmless GET caused the
readiness oracle to abort before the OFF cue. The harness now permits that read;
its fix passed offline checks and was not followed by another hardware trial.
This is not a full ctx4096 stress qualification or production rollout approval.

## Component placement

| Component | Host | Entry point / boundary |
| --- | --- | --- |
| Codex, Chromium, Node browser driver | Ubuntu PC | `scripts/m19-pc-browser.py`, existing `tests/admin/v2/M19ChromiumTests.js` |
| SSH adapter | PC | `tests/admin/v2/M19Remote.js` |
| Backend, private ConfigStore, test HTTP mock | Firefly | `scripts/m19-pc-bridge.py` |
| RKLLM, microphone, voice CPU models | Firefly | Verified candidate process, exclusive hardware window only |
| Root supervisor, watchdog, timer, recovery | Firefly | `scripts/m19-hardware-start.py`, `scripts/m19-hardware-recovery.py` |

Chromium uses the PC X11 environment, UID, executable and browser arguments
recorded in the offline gate. A loopback SSH tunnel exposes Firefly Web Admin
on PC port 18089. All evidence reads, `/proc` accounting, mock log reads and
permitted audio actions go through SSH to Firefly. PC evidence is never used as
a substitute for Firefly resource observations. Chromium and Codex must not run
on Firefly; the hardware plan has no dependency on its DISPLAY/Xorg.

The private mock listens on Firefly localhost. Preserve the original production
mock at localhost:8765; it is a separate service and is not used by the trial.
Do not copy models or overwrite the dirty remote worktree/evidence.

## Offline gate and future preflight

From the PC worktree, with the established SSH control socket and PC DISPLAY:

```sh
python3 scripts/m19-pc-browser.py offline-gate \
  --voice-mode owner-live --artifacts /path/to/private/evidence
```

This runs the existing browser scenario against an explicitly scripted offline
backend: Setup → original proposal → Review → one Apply → restore → ON/OFF
POST/GET. Scripted AI and voice fixtures qualify only this offline gate.
`prepare-only` additionally verifies an unarmed plan and stops its prepared mock;
it does not stop production and does not authorize a trial. Old prepared plans
must not be reused. The retained TTS fixtures failed the frozen wake detector,
so the default synthetic-acoustic hardware preparation is blocked.

A future hardware trial requires a new explicit owner **СТАРТ**. Do not repeat a
successful trial for closure. The trial command creates a fresh offline gate and
checks its age (<300 seconds), browser/source hashes and unchanged PC profile
again before cutover. It verifies candidate build/source/Admin assets, memory,
disk, ports, exclusive NPU/microphone ownership, original production identity,
ConfigStore, release, mock lifetime and the root recovery plan. Failed preflight
must be repaired offline; it cannot justify an experimental production stop.

Qwen, prompts, the 6 GiB admission / 1 GiB reserve, swap, production ConfigStore,
release pointer and permanent systemd unit remain unchanged. No push, merge or
rollout is implied by a gate, prepared plan or local commit.

## Readiness and voice oracle

Owner-live mode waits for an explicit human «готов» before each direction.
`OWNER_WAITING_READY` carries a fresh requestId, direction and private readyFile.
Only after the actual reply does the controller atomically write the matching
`{requestId,on}` ACK on the PC. The driver rejects stale/wrong-direction/reused
ACKs, consumes one ACK, rechecks remote readiness and then emits the speech cue.
Do not generate an ACK automatically or in advance.

Readiness waiting is bounded by min(120 seconds, remote remaining time minus
140 seconds reserve minus 10 seconds). The 140-second reserve comprises 90 for
speech, 5 for diagnostics, 15 for GET click, 15 for confirmation and 15 margin.
An ACK never extends the root deadline. The microphone listens throughout;
the handshake controls harness waiting, not product microphone admission.

Extra empty-body GET /relay/1 during readiness is recorded and rebases the read
baseline. New executions, POSTs, other reads and a shrinking log remain errors.
Live acceptance requires actual STT text, matching CommandEngine input, private
config hash/generation, Safety/domain approval, exactly one successful voice
execution, one correct boolean POST and independently confirmed GET. A human
report or scripted transcript cannot substitute for this evidence.

## Recovery independent of the PC

Before stopping production, verify the armed root supervisor, one-use marker and
600-second timer on Firefly. PC heartbeat runs every 3 seconds; a 15-second stale
lease initiates recovery. Loss of the PC/SSH must not prevent recovery. Hardware
work is limited to 600 seconds; controlled recovery is bounded by 360 seconds.

On failure or completion, close the window, stop the test/audio cgroups, confirm
resource release and restore only the verified original production. Confirm
READY / WAITING_WAKE, original ConfigStore bytes/ownership/modes, release and
unit, and exclusive NPU/microphone ownership. Retain timer/mock evidence until
restoration is confirmed, then clean temporary services. Never kill unknown PIDs
or use a surviving SSH session as the only recovery mechanism.

## Recorded qualification

The final offline gate passed with one Apply, restore, two POSTs and two GETs.
The targeted regression set passed 14 native suites and 36 Python tests;
450 grounding cases produced 164 permitted POSTs and 286 rejections with no HTTP.
The recorded extra-GET regression passed while early POST remained rejected.
These successful tests were not repeated during final closure.

See the [versioned acceptance evidence](evidence/m19/resource-aware-20261014/README.md)
and [finalization record](m19-finalization.md). Full raw archives remain under
`/home/siovush/firefly-m19-control/evidence/nonvoice-final-20261014/` and
`evidence/hardware-owner-budget140-20261014/`; they were preserved, not published.
