# FireflyVoiceAssistantLinux

Text-only C++17 assistant for Firefly ROC-RK3588S-PC (RK3588S, 8 GB), Linux/aarch64, Ubuntu 22.04. Inference is fixed to RKLLM 1.3.0 and Qwen3-4B W8A8 on NPU.

## Build and test
Requires a C++17 compiler, CMake 3.16+, and Make (Ubuntu packages `g++ cmake make`). No third-party test dependencies or model downloads.

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build
ctest --test-dir build --output-on-failure
```

## Components
- `Action`: typed light/entrance-door data and checked compact serialization; performs no hardware work.
- `DeterministicParser`: full-input Russian phrase allowlist, ASCII/Russian case insensitive. Examples: `На кухне свет 40 процентов`, `Включи свет в спальне`, `свет в зале наполовину`. Also accepts exact `сделай`/`поставь свет` forms with the location first (optional `на` before brightness) or after `свет` (required `на` before brightness). Brightness is 0..100 (optional `%` or ` процентов`), `наполовину`, or `максимум`. Surrounding ASCII whitespace and trailing `.`, `!`, `?` are ignored; internal wording is unchanged. Unknown input returns `nullopt`. Door parsing and broader language remain unresolved.
- `SafetyGate`: receives original text and candidate; preserves candidate bytes or returns `U`. Requires an explicit matching room for lights; rejects zero with «включить» forms. Uses whole room words to avoid matching «вокзал» or «кухнейка». D additionally requires an exact open-intent word (`открой`, `откройте`, `открыть`, `открывай`, `открывайте`) plus a `двер` stem and an entrance reference (`входн` stem or adjacent whole tokens `на входе`) in the original input, case-insensitively.
- `ActionValidator`: accepts exact D or L with K/B/G/C and ASCII digits representing 0..100. No trimming, signs, decimal values, or extra output. Leading zeros are accepted consistently with the production reference. U produces no Action.

Production composition: original text → parser → RKLLM if unresolved → gate → validator → Action. `CommandEngine` owns this routing and returns the route plus an optional typed Action. `LlmEngine` allows fake inference in unit tests. The gate uses the validator internally to inspect candidates safely; callers must still validate its result. The CLI prints compact actions only; no hardware execution, audio, or service exists. The gate is limited to the specified light and door checks, not a general natural-language safety classifier.

Unit tests use a small dependency-free runner registered as seven CTest suites (five core suites plus RKLLM C-API stubs and CLI subprocess tests). Failed checks exit nonzero, including Release builds. Public headers are in `include/firefly`, implementations in `src`, tests in `tests`. Reference files under `~/demo_Linux_aarch64` remain read-only.

## Run
```sh
./build/firefly-assistant \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt
```

Enter one command per line. Blank and whitespace-only lines return `U`; `quit`, `exit`, and EOF (Ctrl+D) shut down cleanly. Piped input is supported. Add `--verbose` for startup, bootstrap, prefill/generation, route, and command wall timings on stderr. Application progress and vendor runtime diagnostics go to stderr; compact results go to stdout. The single-process CLI preserves a separate result stream and routes vendor stdout to stderr for the lifetime of the runtime. Request failures return `U` and log the error. Initialization failures print `U`, log the error, and exit nonzero. `--help` needs no model initialization.

## RKLLM lifecycle
`RkllmEngine` owns the handle through a private implementation, including cleanup after partial initialization. It configures the frozen Qwen3 template and reference sampling settings, runs one empty user request, then clears KV with `keep_system_prompt=1`. Each real request repeats this clear before synchronous inference; the model stays loaded. There are no serialized prompt-cache files.

Callback userdata points to the owning implementation: no global bridge is needed. A request mutex serializes inference, a separate mutex protects callback output, and callback exceptions are captured rather than crossing the C API. Raw output is returned only after a successful completed run. CommandEngine removes only ASCII model-output whitespace, matching the reference, then gates against the untouched original input and validates. Engine lifetime must outlast callers; destruction must not race inference.

For unit tests without the SDK/runtime, use a separate build directory:
```sh
cmake -S . -B build/unit -DFIREFLY_BUILD_ASSISTANT=OFF
cmake --build build/unit
ctest --test-dir build/unit --output-on-failure
```

Real NPU smoke tests are manual and separate from CTest. The SDK, runtime, model, and frozen prompt stay outside this repository. No dependencies or models are downloaded by CMake. The parser compiles its unchanged regex grammar once; the existing suite now takes about 0.08 seconds (previously about 7.1 seconds). SafetyGate now requires all three explicit door cues for D. The explicit phrase «на входе» is accepted as an entrance reference; all three door cues remain mandatory. See the failure and input policy below.

## Failure, input, and deadline policy
`CommandEngine` returns U for invalid input, thrown runtime errors, malformed outputs, and SafetyGate rejection. Results include distinct raw/normalized model output, final Action, error, route, and latency fields. No previous result is reused. The runtime clears callback state before KV clearing and inference, bounds callback output to 4096 bytes, and rejects unknown callback states, missing finish, error callbacks, and invalid callback sequences. `RKLLM_RUN_WAITING` is a documented normal state.

Input must be nonempty valid UTF-8, at most **192 bytes**, with no NUL or non-whitespace ASCII control bytes. This allows approximately 96 Cyrillic letters, sufficient for short room commands, while leaving a conservative margin around the frozen prompt (788 observed bootstrap tokens) in the 1024-token context. This is a byte cap, not a token guarantee. The CLI stores at most 193 bytes per line and drains excess bytes before returning U, so the next command remains independent. SafetyGate still receives the unmodified original accepted input.

RKLLM 1.3.0 declares `int rkllm_abort(LLMHandle)` and `int rkllm_is_running(LLMHandle)`. A joined watchdog requests abort after **30 seconds**, including bootstrap; CLI `--timeout-ms N` overrides this. It does not detach inference or destroy the handle while work is running. After a deadline/run/KV/callback fault, the engine remains not-ready and fallback returns U until process restart; the independent fast path remains usable. Invalid user input and malformed model output do not poison an otherwise healthy runtime.

Abort is best effort: the header does not guarantee bounded return time. The caller still waits for synchronous `rkllm_run` and the watchdog to finish. A stuck vendor run/abort can therefore still block shutdown; model initialization and KV clear also have no interruptible deadline. CLI requests taking at least 2000 ms produce a stderr warning. Timing and detailed runtime logs are available with `--verbose`.

## DEV regression on the NPU
```sh
./build/firefly-regression \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --system-prompt /home/firefly/demo_Linux_aarch64/prompt_compact_v5_short_system.txt \
  --data tests/data/dev.tsv --verbose \
  > build/dev-regression.tsv 2> build/dev-regression.log
```

This executes the production CommandEngine. The 48-row DEV dataset is balanced between 24 valid actions and 24 expected U responses; it is not a hidden evaluation. Per-case routes/final results and aggregate metrics go to stdout; original input, raw model output, normalized output, final result, and errors remain distinguishable in stderr diagnostics. Exit status is 0 for a clean pass, 1 for regression failures, and 2 for harness/startup errors. See `tests/data/README.md` for metric definitions.

Unit tests need no NPU. Wrapper/CLI tests link stub C functions against the exact SDK header and require Python 3 for subprocess checks; the runtime library is not linked to these tests. A core-only build runs the five core suites without the SDK. No dependency is downloaded automatically.
