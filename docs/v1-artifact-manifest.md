# Firefly V1 Prototype — Artifact Manifest

## Source

- Git tag: `v1-prototype`
- Git commit: `2cd84743b678851caf99307055135e019e3f210f`
- Platform: Firefly ROC-RK3588S-PC
- Architecture: aarch64
- OS: Ubuntu 22.04

## LLM

Production model:

`Qwen3-4B_W8A8_RK3588_3core.rkllm`

- RKLLM runtime: 1.3.0
- RKNPU driver: 0.9.8
- Context: 1024
- SHA256:
  `d806976a3d77f2252a6abe40a5fd7d9b1006a90f671992923ac2c1542456ba8b`

Runtime library:

`librkllmrt.so`

## STT

- sherpa-onnx: 1.13.7
- Russian Zipformer INT8
- CPU inference

Files:

- `encoder.int8.onnx`
- `decoder.onnx`
- `joiner.int8.onnx`
- `tokens.txt`

## VAD

`silero_vad.onnx`

SHA256:

`9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6`

## Microphone acceptance

Tested with:

`Seeed Studio reSpeaker XVF3800 4-Mic Array`

ALSA device:

`plughw:CARD=Array,DEV=0`

V1 live smoke passed at approximately 2 meters:

- «Включи свет на кухне»
- «Выключи свет на кухне»
- «Поставь свет на кухне на сорок процентов»

## External backup layout

Expected backup:

`FireflyBackups/v1-prototype/`

- `rkllm/`
- `runtime/`
- `stt/`
- `source/`

Large models and runtime artifacts are intentionally NOT stored in Git.

## Exact runtime hashes

```text
6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6  /home/firefly/demo_Linux_aarch64/lib/librkllmrt.so
69a347ddf7e5d39452d17f059dfedcd8c9471ca1953c320cb04ca19626149fdb  build/stt/model/encoder.int8.onnx
3977101b53640e50dff4274988ef754cd7ea539ae480baed0d18fc4966ad6917  build/stt/model/decoder.onnx
a6f550b56375326ee4a67dcac2cac027035e2dc137c08fff3557f4ffdc5cbc3e  build/stt/model/joiner.int8.onnx
93bbbc0bae6b78c0bbb743d4aa9fded3bb5ff3aac5f0200e3a769a5a05e0fdf6  build/stt/model/tokens.txt
9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6  build/stt/vad-ab/silero_vad.onnx
5997c35aa68c8843b2ddfa6208b6961a07f171fa636633dd8941270e1aaacf5f  build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libsherpa-onnx-c-api.so
d860f5968f5a1ed63533e9ed198aa747ca9fe289028129877f428556089f6874  build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libonnxruntime.so
```

## V1 production wake artifact (post-prototype integration)

Small versioned artifact: `assets/wake/amina-v3.tflite` (60,912 bytes).
SHA256: `bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1`.
Frozen cutoff 0.99, mean window 7, XVF3800 ASR channel index 1.
Runtime checks verified model bytes and quantization metadata. Packaging, pinned
worker dependencies and current acceptance status: [Amina V1](v1-amina-wake.md).
