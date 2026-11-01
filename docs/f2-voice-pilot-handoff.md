# Accepted F2 voice pilot — integration handoff, 2026-11-01

The owner accepted the F2 conversational pilot and asked to preserve it for
later integration. **The pilot is not wired into the main assistant by this
change.** Its source, tools, reports and checksums are frozen in the private
archive below, with a verified HDD copy. The associated
[Amina/Amila revision](amina-amila-release.md) is promoted independently into
normal release inputs.

## Frozen implementation

| Item | Value |
| --- | --- |
| PC working archive source | `/home/siovush/qwen-f2-stream-20261101` |
| Source/evidence archive | `/home/siovush/qwen-f2-stream-20261101-evidence.tar.gz` |
| HDD archive | `/media/siovush/Transcend/qwen-f2-stream-20261101-evidence.tar.gz` |
| Archive bytes | 13,764,479 |
| Archive SHA256 | `33564079805982b30d9e945e0c54b87ea3ae00f21c131319cb791b43bee4c52b` |
| Entries verified against SHA256SUMS | 259 files |
| ARM64 `chat-f2` SHA256 | `6d1b8a1a99668b3ce9e81bafec986f16db07ed811ddf1363da03458850fc1122` |
| Native assets manifest SHA256 | `76f5e78bfd033e1164a28109a8bc136bfb926bc641e8acd101bd390256b8cfc7` |
| Unchanged product capture/KWS/VAD/STT source | `9ca85f8caae59a19c6bf775fe314919710f124f9` |

`README.txt`, `REPORT.json`, `native/final-verification.json`, implementation
sources and local test tools are in the archive. Hash-check it before use;
extract into a new private directory. Keep the PC and HDD copies. The archive
contains the pilot implementation, not a production release or the large
Qwen/STT/TTS weights. Raw owner microphone input remains private on Firefly;
it is not in Git or the archived source bundle. Paths are an operator inventory,
not downloadable GitHub attachments. No source archive or audio is uploaded by
this documentation change.

## Accepted profile

- Voice **F2 / SID 1**, Supertonic 3 INT8, Russian, existing sherpa-onnx 1.13.7;
  CPU 4 threads, 5 synthesis steps, speed 1.0. Do not substitute another voice.
- Qwen3-4B W8A8, context 4096, thinking forced OFF, default answer limit 128
  tokens. History exists within the session and clears after 3000 KV tokens.
- TTS loads once per session. Qwen text is split at sentence/clause boundaries;
  bounded queues hold 32 text chunks and two rendered audio chunks. Generation,
  synthesis and native ALSA playback overlap. Code/reasoning blocks are silent.
- Microphone: `plughw:CARD=Array,DEV=0`, stereo S16_LE 16 kHz, ASR channel 1.
  Existing KWS/VAD/STT classes remain unchanged; Amina model SHA is
  `24000c0e3f9fd3249f56e5f836719aba8f7a3f23c1f4a2c571bdf0bf6cc8a1dd`.
- Playback: `plughw:CARD=rockchipes8388,DEV=0`, 44.1 kHz mono PCM.
  Capture is drained while processing/speaking plus 500 ms echo tail.
  Voice interruption of speech is not implemented.
- Existing 6 GiB admission / 1 GiB reserve; transient limits MemoryMax=6G,
  Swap=0, Tasks=128, NOFILE=256. Sessions are bounded to 1–120 minutes.

This isolated opt-in pilot temporarily stops the main service to own the
microphone/NPU, and restores it on exit. It has no ConfigStore/device executor
integration and cannot control MQTT or HTTP devices. The future product must
retain the configured command path and SafetyGate; conversational output must
not become device commands. Do not start another pilot alongside an active one.

## Measured evidence and limits

Historical final checks on the pinned executable/assets:
620 host sentence checks; 19 Speaker lifecycle checks under ASAN/UBSAN/LSAN;
unchanged production WakeVoiceGating; real ARM64 ELF/dependency closure;
actual KWS → STT → Qwen → F2 PCM replay; 90 seconds no-wake with no STT/Qwen
requests or audio; cancellation during playback, PIDs gone, next replay succeeds.
104 asset hashes matched; minimum MemAvailable was 1993.945 MiB.

In the final two-turn stream, first PCM reached ALSA after 8.682 seconds for
the long answer (Qwen finished at 26.603 seconds) and 4.149 seconds for the short
answer. Observed stream underruns: zero. These are specific samples, not a
latency guarantee for every question; first PCM is not proof of audible quality.
The owner separately reported liking the running voice pilot.

NOT_RUN: controlled new live-microphone acceptance, acoustic echo/distance/noise
matrix, day-long soak, conversational product integration/package/activation.
ThreadSanitizer could not run because of its runtime memory-mapping error;
no host security setting was changed. Exploratory two/three-step synthesis
profiles failed quality limits and are not the accepted profile.

## Existing opt-in operator usage

Only in an owner-authorized test window, after ensuring no pilot already owns
the microphone/NPU, the existing launch command is:

```sh
ssh -t firefly@100.91.173.108 'python3 -B /home/firefly/qwen-f2-stream-20261101/launch.py --mode voice --minutes 30'
```

Wait for `DIALOGUE_READY` and `WAITING_WAKE`. Exit with Ctrl+C, or say the wake
word followed by «заверши разговор». Local session logs are under
`/home/firefly/qwen-f2-stream-20261101/sessions/<UTC-id>/`.
Recovery after a failed/closed experiment is documented in the private README:

```sh
ssh firefly@100.91.173.108 'sudo -n systemctl stop firefly-qwen-chat-lab.service; sudo -n systemctl start firefly-voice-assistant.service'
ssh firefly@100.91.173.108 'sudo -n firefly-wake-debug status'
```

Verify readiness and `WAITING_WAKE`; do not run recovery during a session the
owner still wants. No command above is executed as part of this Git change.

## Next integration task, to discuss with owner

Reuse this frozen implementation/profile in the existing assistant lifecycle;
define when to answer a question versus execute a configured command; preserve
original-input grounding/SafetyGate, setup resource ownership, cancellation,
echo suppression and recovery. Validate the integrated runtime before a normal
release. Do not copy the pilot as a second permanent service. Integration,
new product behavior and device installation require the later task requested
by the owner; none is implied by preserving these references.
