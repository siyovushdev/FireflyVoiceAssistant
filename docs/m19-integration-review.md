# M19 integrated exhibition review — 2026-10-13

**READY_FOR_CHIEF_ENGINEER_REVIEW.** Tested clean application code:
`dd1ffa204a5757f32a13c7943a6ed05df68b0f49`. This ordinary merge incorporates
production master `818157ec0eef7eb7cb458983db2022af7f7c78ea` into the existing
PR #15 branch. The later evidence/runbook commit changes documentation only.
No rebase, force-push, PR merge or production update is part of this work.

The complete **offline** journey passes through actual C++ HTTP Admin,
ConfigStore, Vue/Chromium, language index, shared SafetyGate and HTTP transport.
AI replies are scripted; PCM/KWS/VAD/STT are synthetic seams feeding the actual
wake loop and voice execution route. This is not real Qwen or physical voice.

| Stage | Fresh proof |
| --- | --- |
| Empty store → AI Setup → Cabinet/Lamp proposal | Visible `лампа`, `лампу`, `в кабинете`; boolean power and ON/OFF; Accept remains draft only |
| Explicit HTTP Connection, action bodies and Observe | POST `/relay/1` with on=true/false; GET `/relay/1`, on→power; missing facts ask questions, probe sends zero HTTP bytes |
| General Review → one final Apply | One setup Apply per desktop/mobile scenario; no I/O before Apply; active/stored versions agree |
| ON/OFF through synthetic voice seam | INDEX_EXACT, SafetyGate PASS, source=voice, exactly one correct POST each; state is Predicted |
| Explicit independent Observe | One GET after each command makes state Confirmed; contradiction/readback failures remain visible |
| Negatives and persistence | Unsafe/ambiguous actions, unknown room and no-wake produce zero I/O; restart returns Unknown; GET confirms again; LKG recovery sends zero HTTP |

Cabinet desktop and additional no-zone mobile each recorded **2 POST / 7 GET**.
The seven reads include ON/OFF, a contradictory observation, three failed reads
and the read after restart. The rejected historical owner-profile case recorded
**zero Apply / zero HTTP**. Multi-device/location ambiguity is additionally
covered by **450/450 grounding cases**: 164 allowed with 164 correctly addressed
POSTs, 286 denied with zero HTTP, across both protocols and six request lanes.

Validation: **87/87 CTest** (362.24 s), **41/41 frontend**, typecheck/build/offline
asset gate; all existing Chromium suites plus M19 passed. M19 Chromium took
97.47 s. Worker `ldd` contains only standard libraries, no RKLLM/sherpa/ALSA.
See [fresh proof and screenshots](evidence/m19/integration-20261013/README.md).
The documented reproduction uses `build-m19-integrated`; full commands are in
[the demo guide](m19-demo.md#воспроизведение-инженерных-проверок).

Integration required no new product implementation. The merge conflict selected
the master's identical grounding fixture under its canonical name; duplicate
CMake registration was removed. SafetyGate, CommandEngine, voice/wake sources,
models and existing release mechanisms remain unchanged from master. Existing
M19 product delta is the reviewed device language-profile prompt/decoder and
Vue speech-form/warning display. Current docs replace the unsafe pre-hotfix live
candidate selection and occupied mock port with the integrated candidate and
separate test ports. Original evidence, including the archived old M19 build,
is preserved; historical failed wake is not relabeled as success.

Production stays on `818157ec`, PID **715461**, original start time and unit.
All ConfigStore bytes, owners, modes, timestamps, revision 2 and hash
`688690e892d2f59413cdc2f7d81f1e129ab9ac0dd247f84b2898d74c3c3cdd18` are unchanged.
Only that process owns NPU/microphone. No live Apply, device command, runtime
stop/restart, deployment or second model/microphone owner was performed.

Remaining: Chief Engineer review, then **one separately authorized physical
acceptance** with a fresh test store and localhost mock. Historical genuine Qwen
proposal and real text acceptance remain valid historical evidence, not a fresh
pass of this integrated build. Real wake/STT/voice acceptance is open under #18;
source parity with the working production baseline does not prove acoustics.
The agent must prepare preflight/watchdog, run AI Setup, preserve original forms,
check the HTTP contract and perform only test Apply. Owner only confirms audible
cues and says INLINE «Амина, включи лампу в кабинете», then SEPARATED
«Амина» → end of BEEP → about 0.5 s → «выключи лампу в кабинете».
See [bounded live plan](m19-demo.md#один-последующий-owner-gate-настоящий-qwen-и-respeaker).
No thresholds/model tuning or new wake investigation is authorized here.
Keep PR #15 Draft; do not close #14/#16/#18 or call the MVP voice-accepted yet.
