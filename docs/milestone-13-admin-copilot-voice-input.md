# Milestone 13 — Local Voice Input for Admin Copilot

Production remains Qwen3-4B W8A8 RK3588, context **1024**. No 4096 work, no
HardwareExecutor, no wake word, no automatic Copilot / Save / Apply after speech.

## Architecture

```text
browser microphone (laptop)
      ↓
AudioContext PCM capture (mono)
      ↓
canonical PCM WAV (bounded ≤15 s)
      ↓
POST /api/admin/transcribe  (X-Firefly-Admin: 1, audio/wav)
      ↓
decodeWavToSttPcm → linear resample → 16 kHz float
      ↓
long-lived SherpaRecognizer (Russian Zipformer)
      ↓
{ok, text, duration_ms, stt_ms, copilot_started:false}
      ↓
existing Copilot textarea (manual edit → Предложить изменения)
```

This ingress is **not** the ALSA H340 assistant loop. Shared pieces are the
existing `SherpaRecognizer` / Zipformer model and the new `decodeWavToSttPcm`
helper. Lifecycles stay separate: browser upload never enters CommandEngine.

## Secure context

`getUserMedia()` requires a browser secure context. Firefly Admin is plain HTTP
on the device; open it via SSH tunnel from the laptop:

```sh
ssh -L 18080:127.0.0.1:8080 firefly@<firefly-ip>
```

Then open `http://localhost:18080` (localhost is treated as a secure context).

If `window.isSecureContext == false`, the UI does not keep prompting for the
microphone; it shows a human `insecure_context` message. Production TLS/PKI is
out of scope for M13.

## API

`POST /api/admin/transcribe`

- Guard: `X-Firefly-Admin: 1`
- Body: raw WAV bytes, `Content-Type: audio/wav` (also `audio/x-wav`, `application/octet-stream`)
- Limits: mono PCM S16; rates 8/16/22.05/44.1/48 kHz; ≤15 s; ≤ ~1.44 MB body
- Concurrent recognition: one at a time (`stt_busy`)
- Audio is not written to disk

Success:

```json
{"ok":true,"text":"…","duration_ms":2340,"stt_ms":580,"saved":false,"applied":false,"copilot_started":false}
```

`GET /api/status` includes `stt_ready`.

## Lifecycle

- Startup: optional `--stt-model-dir` loads one long-lived Admin Zipformer.
- Ready / unavailable: Web Admin keeps working; mic shows controlled `stt_unavailable`.
- Missing Firefly H340 does **not** block browser transcription.
- `--voice` ALSA loop remains a separate assistant path with its own recognizer.
- Shutdown: HTTP clients join before model destruction (existing demo teardown).

## Tests

- Deterministic `AdminTranscribe` (WAV policy, fake STT, API guard, no store mutation)
- UI model: voice state machine + mic/secure-context error mapping
- Full CTest
- Separate real sherpa-onnx replay of an existing eval WAV through `/api/admin/transcribe`

## Live browser mic

Agent cannot fully exercise laptop microphone permission UI. Manual steps are in
the Milestone 13 delivery note / final report.
