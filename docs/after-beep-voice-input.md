# Voice input after the beep

The appliance now defaults to `--wake-command-mode after-beep`:

1. Say «Амина».
2. Wait for the wake beep to finish.
3. Say the command or operational question.

KWS admission immediately starts acknowledgement. Room speech cannot select an
inline branch. All pre-beep, beep and existing echo-drain PCM is discarded before
a fresh VAD session. A failed/stalled acknowledgement closes that admission and
returns to wake detection; it does not silently start listening for a command.
This mode requires the beep enabled. It deliberately does not accept a command
spoken together with the wake word. No setting is written to ConfigStore.

The prior automatic inline/separated behavior remains an explicit
`--wake-command-mode auto` option. Programmatic replay callers retain that prior
WakeConfig default; the real appliance CLI chooses after-beep by default. Existing
automatic-mode prefix, history, stop and overflow regressions remain intact.

Within an already KWS-admitted session, an exact wake-only transcript such as
«Амина» or «Амина, Амина» returns `WAKE_ONLY`, without CommandEngine, Qwen,
executors, execution history or F2 refusal. In after-beep mode it starts another
beep and fresh command window. It does not strip names from ordinary commands,
repair STT, fuzzy-match names or grant wake authority to text/Admin input.
Existing start/max-speech deadlines, stop/generation guards, single capture,
original-text grounding and SafetyGate remain authoritative.

## Direction experiment

`--wake-focus-socket /private/owned/directory/control.sock` is optional and off by
default. `WakeDirectionClient` sends only an allowlisted lifecycle name and a
monotonic timestamp using bounded nonblocking Unix datagrams, before journal I/O.
There is no audio, transcript, action, target, credential, background sender,
queued retry or helper process launch. Missing/full endpoints do not stall capture.

`experiments/xvf3800-focus.py` is an operator experiment, not a packaged or
automatically installed service. It can consume direct datagrams using
`watch --socket PATH --pid ADMITTED_APP_PID --state-dir PRIVATE_SNAPSHOT_DIR`.
The receiver checks kernel SCM_CREDENTIALS for the admitted PID/UID1000, process
birth, bounded lifecycle, order and freshness. It accepts no requested angle or
arbitrary hardware command from the application. A private0700 socket parent is
required. The finite session limit is600seconds, fixed hold at most20seconds.

The real tested Seeed2886:001a firmware is2.0.10, stereo16kHz/S16_LE, right ASR
channel1 routed to `[7,3]`, ASROUT1. With a fresh stable speech direction, the
experiment temporarily points fixed beams there and selects processed ASRbeam1
on that same right channel `[7,0]`; auto beam3 can still select the free beam.
It restores the saved route/mode/angles with readback on processing, timeout,
shutdown or process death. Supervised hardware windows additionally require an
independent ExecStopPost restoration. No flash/save/reboot, gain/model/threshold,
driver detach, rate, format or concurrent PCM capture is allowed by this tool.

Protocol metadata is pinned to the [official Seeed host interface](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/4b49bfd19977c63cf6e90dfe9e5827e74e20bf6d/python_control/xvf_host.py).
The installed libusb is used; no Python package/model installation is needed.
Direction is not speaker identity. Same-direction speakers and overlapping speech
remain limitations; mechanical control/readback alone does not prove acoustic
improvement. Acceptance results and exact temporary-window identities are reported
separately. No permanent unit, release pointer or production activation is implied.

Tests cover after-beep room/inline exclusion, wake-only controls, failed beep,
timeouts/stop, original automatic behavior, bounded datagrams/backpressure/kernel
credentials and controller selection/restoration. Synthetic capture/model seams
are not owner or acoustic acceptance. Device-state periodic telemetry is a separate
deferred task; existing freshness and truthful voice responses are unchanged.
