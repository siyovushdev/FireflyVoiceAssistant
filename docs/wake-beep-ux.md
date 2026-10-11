# Issue #12 — audible separated-wake acknowledgement

Base: `df812bc` (`origin/master`, merged M18.6). Branch: `fix/wake-beep-ux`.
Implementation follows the [Chief Engineer plan](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/12#issuecomment-6107526684).

## Behavior and architecture

The default acknowledgement is a 400 ms, 880 Hz, 16 kHz mono signed 16-bit PCM
sine, with 30% peak amplitude and 10 ms linear fades at both ends. This matches
the duration/amplitude the owner previously heard through ES8388 analog AUX.
Audibility of this implementation still requires owner hardware acceptance.

`WakeAcknowledgementConfig` and `makeWakeAcknowledgementPcm` live in the voice
core, so PCM/settings tests need no ALSA, models or NPU. The ALSA owner uses this
PCM and keeps its nonblocking writes, partial-write handling, nonblocking drain,
one-second playback deadline, cancellation and RAII cleanup. There is no worker
thread, mixer change, subprocess, volume auto-detection or additional dependency.
The deadline is checked by polling; it is not a hard bound on a vendor setup/close
call or on a capture read that never returns.

Inline wake + command retains its original PCM path and never starts BEEP.
Separated wake drops continuation history, capture PCM during playback and the
existing 250 ms post-playback capture/acoustic drain. A fresh segmenter session
clears pre-roll before listening. The command-start timeout starts after the
acknowledgement and drain; its default remains 5 seconds plus the existing VAD
start confirmation allowance. A stalled acknowledgement is cancelled after one
second and follows the same drain/admission path as playback failure.

Absent/failed playback remains diagnostic: `ACKNOWLEDGEMENT_FAILED`,
`playback_failed`, and degraded overall health, while wake and STT admission
remain independent of output. Silent/muted playback cannot be inferred from ALSA
success. Intentionally disabled playback opens no playback device, logs no BEEP
states, and proceeds directly to listening after separated wake. Wake gating
remains enabled unless separately changed by its existing option.

## Startup controls and deployment contracts

`firefly-demo` accepts:

| Option | Default | Accepted values |
| --- | --- | --- |
| `--wake-beep-enabled` | `true` | exactly `true` or `false` |
| `--wake-beep-volume-percent` | `30` | whole decimal integer, 1 through 30 |

Volume is PCM amplitude, not mixer gain or perceived loudness. Lower levels may
be inaudible on AUX. Use the explicit enable flag to disable playback. Volume
validation also applies when disabled. Invalid values fail before ConfigStore,
HTTP listener, model or device initialization. Effective settings are logged as
`VOICE_ACK enabled=... volume_percent=...` when the voice owner opens capture.

The existing ConfigStore v1/v2 schemas are closed (`additionalProperties: false`)
and contain no audio settings; the Admin API has no playback-settings endpoint.
These are process/startup options, with no new persistence path. Web Admin
controls are **deferred**, and no Web Admin configurability is claimed. ConfigStore
schema, hardware policy, wake thresholds, STT/VAD parameters, language parser,
SafetyGate, model prompt/configuration, protocol and runtime command processing
are unchanged. The existing service template, generated service and dialogue
helper keep their argv: omission of the new options selects audible defaults.
An owner can add the validated options to the reviewed startup command in a
separately authorized deployment; the dialogue helper has no new passthrough.

## Verification

All build output is under
`/home/firefly/FireflyVoiceAssistantLinux-wake-beep/build`. Existing SDK, sherpa
libraries/header, wake Python and identical frontend dist are read-only inputs.
No dependencies or models were downloaded, and no real RKLLM/STT model was copied.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_VOICE=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python \
  -DFIREFLY_ADMIN_DIST_DIR=/home/firefly/FireflyVoiceAssistantLinux/src/admin/frontend/dist \
  -DFIREFLY_NODE_EXECUTABLE=/usr/bin/node
cmake --build build -j2
PYTHONDONTWRITEBYTECODE=1 ctest --test-dir build -j1 --output-on-failure \
  -R '^(WakeAcknowledgementPcm|AlsaAcknowledgement|WakeSessionUx|SeamlessWake|WakeVoiceGating|VoicePipeline|WakeAcknowledgementOptions|VoiceRecovery|VoiceRecoveryIntegration|ProductionStartupContract|ProductionStartup)$'
PYTHONDONTWRITEBYTECODE=1 ctest --test-dir build -j1 --output-on-failure
git diff --check
```

Focused coverage: 13 PCM/settings cases; 12 fake ALSA cases (including deadline,
partial writes/drain, five error paths, cancellation and destructor); 13 wake UX
cases (416 ms playback, successful/failed/absent/stalled/disabled output, 300 ms
pre/post-roll echo isolation, late command start, timeout, shutdown during beep
and drain); 21 invalid CLI cases, help, and five real-entrypoint startup/health
scenarios with fake devices/RKLLM. Existing seamless tests cover inline admission,
PCM recovery, safety and shutdown with the longer mock acknowledgement.

The serial full CTest includes all Config* suites, three Chromium acceptance
suites, startup/recovery and ProductionPackaging. Packaging tests operate only
inside disposable fixture trees/private stores, with tiny fake model files;
they do not stage a production release or install/activate anything in `/opt` or
`/etc`. Real playback, microphone/NPU smoke, staging and deployment are excluded.

Focused CTest passed **11/11**, with zero failures, in **31.34 seconds**.
Final full-suite results are recorded in the Draft PR and engineering report.

## Planned owner hardware smoke (not performed)

Requires Chief Engineer review and separate owner authorization for staging,
cutover and any service action. Do not start a second capture/NPU owner beside
production.

1. Validate the approved source commit, clean build identity, package provenance
   and rollback artifacts; stage only under the separately approved procedure.
2. On the AUX-connected speaker verify separated «Амина»: audible smooth BEEP,
   `BEEP_START` → `BEEP_END` → `LISTENING_COMMAND`. Record monotonic timings and
   effective enabled/volume settings. Check 400 ms playback plus 250 ms drain.
3. Follow with a safe unsupported command such as «Какая погода?»; confirm STT
   contains command speech, with no acknowledgement/wake contamination, and no
   device action. Repeat near the five-second start deadline and with no speech;
   require correct timeout/rearm and no replay.
4. Verify inline «Амина, какая погода?» remains uninterrupted, with no BEEP,
   complete STT and no device action. Check consecutive sessions and pre-roll.
5. Verify disabled BEEP still requires wake and admits the same safe command.
   Test volume within the approved range; do not change the mixer automatically.
6. Under separately approved device-test conditions verify absent/failing output
   retains diagnostics/degraded health and command admission without a restart
   loop. Muted output may return ALSA success: confirm admission remains intact.
7. Check owner-authorized shutdown during playback/drain, health, zero unexpected
   executions and ConfigStore integrity. A reboot check needs separate approval.

## Production rollback checklist (not performed)

- Retain the current M18.6 release, `current` target, prior release marker and unit
  backup; record hashes, health and the ConfigStore integrity report before cutover.
- Preserve `/home/firefly/FireflyVoiceAssistantLinux/build/firefly-demo`, its
  original checkout and dependencies as first-cutover rollback; also preserve
  the M18.6 feature checkout. Do not rebuild either checkout for this hotfix.
- Any failure of audibility, echo isolation, admission, timeout, health or safe
  unsupported behavior blocks acceptance. Use the existing M18.6 documented
  rollback procedure after explicit owner authorization; avoid in-place edits.
- Restore the reviewed release pointer/unit if required, start only the intended
  owner, and recheck health, wake/STT, unsupported command and store integrity.
  This hotfix requires no schema migration or configuration rollback.

## Self-review limits

The tests establish PCM and control-flow contracts, not physical AUX loudness or
room-specific echo decay. Existing drain/pre-roll/wake/STT settings were preserved,
not tuned. Neither acoustic echo cancellation nor persistent UI controls were
added. No production files, service restart/reload/reboot, real hardware playback,
PR merge or production staging/installation are part of this change.
