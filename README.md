# FireflyVoiceAssistantLinux

Text-only C++17 core for Firefly ROC-RK3588S-PC (RK3588S, 8 GB), Linux/aarch64, Ubuntu 22.04. Future inference is fixed to RKLLM 1.3.0 and Qwen3-4B W8A8 on NPU.

## Build and test
Requires a C++17 compiler, CMake 3.16+, and Make (Ubuntu packages `g++ cmake make`). No third-party test dependencies or model downloads.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Components
- `Action`: typed light/entrance-door data and checked compact serialization; performs no hardware work.
- `DeterministicParser`: full-input Russian phrase allowlist, ASCII/Russian case insensitive. Examples: `На кухне свет 40 процентов`, `Включи свет в спальне`, `свет в зале наполовину`. Also accepts exact `сделай`/`поставь свет` forms with the location first (optional `на` before brightness) or after `свет` (required `на` before brightness). Brightness is 0..100 (optional `%` or ` процентов`), `наполовину`, or `максимум`. Surrounding ASCII whitespace and trailing `.`, `!`, `?` are ignored; internal wording is unchanged. Unknown input returns `nullopt`. Door parsing and broader language remain unresolved.
- `SafetyGate`: receives original text and candidate; preserves candidate bytes or returns `U`. Requires an explicit matching room for lights; rejects zero with «включить» forms. Uses whole room words to avoid matching «вокзал» or «кухнейка». D is preserved under the specified initial rules.
- `ActionValidator`: accepts exact D or L with K/B/G/C and ASCII digits representing 0..100. No trimming, signs, decimal values, or extra output. Leading zeros are accepted consistently with the production reference. U produces no Action.

Future composition: text → parser → RKLLM if unresolved → gate → validator → Action. The gate uses the validator internally to inspect candidates safely; callers must still validate its result. No orchestration, RKLLM integration, hardware execution, audio, or service exists yet. The initial gate is limited to the two specified semantic rules, not a general natural-language safety classifier.

Unit tests use a small dependency-free runner registered as four CTest suites. Failed checks exit nonzero, including Release builds. Public headers are in `include/firefly`, implementations in `src`, tests in `tests`. Reference files under `~/demo_Linux_aarch64` remain read-only.
