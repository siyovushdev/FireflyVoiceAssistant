# FireflyVoiceAssistantLinux

C++17 text and offline voice assistant for Firefly ROC-RK3588S-PC (RK3588S, 8 GB), Linux/aarch64, Ubuntu 22.04. Inference is fixed to RKLLM 1.3.0 and Qwen3-4B W8A8 on NPU.

## Build and test
Requires a C++17 compiler, CMake 3.16+, and Make (Ubuntu packages `g++ cmake make`). No third-party test dependencies or model downloads.

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Components
- `GenericAction`: stable action/target IDs and named `int64` arguments; `Unsupported` is a separate non-executable result.
- `ConfigSnapshot` / `DomainView`: immutable validated configuration retained for process lifetime. Rooms/aliases, brightness domain and entrance target are config-authoritative.
- `LanguageCompiler` / `LanguageIndex`: config-derived immutable action atoms, exact scalar extraction and explicit ambiguity rejection per RuntimeGeneration; [architecture and measurements](docs/language-index-architecture.md), [Admin language profiles](docs/language-index-admin-flow.md).
- `DeterministicParser`: conservative existing Russian grammar, producing GenericAction directly without guessing.
- `LlmContext`: deterministic static instructions plus domain data generated from the same snapshot. No administrator-edited system prompt.
- `CompactActionDecoder`: production schema-driven `[integer]` / `[]` / `null`, locked to deterministically selected IDs; one warm command context.
- `StructuredDecoder`: legacy and complex-schema strict JSON ingress into GenericAction/Unsupported. Rejects malformed output, duplicate keys, prose, unknown identities, invalid arguments and out-of-domain values without repair.
- `SafetyGate`: checks original text and typed candidate. Explicit target grounding, contradictory light-state rejection and explicit entrance-door opening requirements remain compiled deterministic policy.
- `CommandEngine`: LanguageIndex → legacy parser or RKLLM → typed candidate → SafetyGate → config-backed domain validation → typed final result.
- `CompactV1Adapter`: outward CLI/debug/regression compatibility only. `D`, `L|K/B/G/C|N`, `U` are not the model output or internal semantic representation.

Production execution requires explicit bindings; V1 acceptance uses the loopback HTTP mock. The gate enforces the documented specific rules; it is not a general natural-language safety classifier. See [Milestone 8](docs/milestone-8-config-driven-llm.md) and the [voice guide](docs/live-voice.md).

Public headers are in `include/firefly`, implementations in `src`, tests in `tests`. Reference files under `~/demo_Linux_aarch64` remain read-only. JSON v3.12.0 and its MIT license are vendored; builds download nothing.

## Run
```sh
./build/firefly-assistant \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --config config/current-m4.json
```

Enter one command per line. Blank and whitespace-only lines return `U`; `quit`, `exit`, and EOF (Ctrl+D) shut down cleanly. Piped input is supported. Add `--verbose` for startup, bootstrap, prefill/generation, route, and command wall timings on stderr. Application progress and vendor runtime diagnostics go to stderr; compact results go to stdout. The single-process CLI preserves a separate result stream and routes vendor stdout to stderr for the lifetime of the runtime. Request failures return `U` and log the error. Initialization failures print `U`, log the error, and exit nonzero. `--help` needs no model initialization.

## RKLLM lifecycle
`RkllmEngine` owns the handle through a private implementation, including cleanup after partial initialization. It configures the frozen Qwen3 template and reference sampling settings, runs one empty user request, then clears KV with `keep_system_prompt=1`. Each real request repeats this clear before synchronous inference; the model stays loaded. There are no serialized prompt-cache files.

Callback userdata points to the owning implementation: no global bridge is needed. A request mutex serializes inference, a separate mutex protects callback output, and callback exceptions are captured rather than crossing the C API. Raw output is returned only after a successful completed run. CommandEngine decodes exactly one JSON result (JSON whitespace is allowed), then gates against the untouched original input and validates. No output repair or compact ingress is used. Engine lifetime must outlast callers; destruction must not race inference.

For unit tests without the SDK/runtime, use a separate build directory:
```sh
cmake -S . -B build/unit -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_CONFIG_TOOLS=ON
cmake --build build/unit
ctest --test-dir build/unit --output-on-failure
```

Real NPU smoke tests are separate from CTest. The SDK, runtime and model remain outside this repository. Prompt text is generated locally from validated config. To inspect it, build with `-DFIREFLY_BUILD_CONFIG_TOOLS=ON` and run `firefly-llm-context config/current-m4.json`. Manual `--system-prompt` overrides are rejected.

## Failure, input, and deadline policy
`CommandEngine` returns U for invalid input, thrown runtime errors, malformed outputs, and SafetyGate rejection. Results include raw model output, structured decode status, typed candidate, post-gate candidate, final result, error, route and latency. No previous result is reused. The runtime clears callback state before KV clearing and inference, bounds callback output to 4096 bytes, and rejects unknown callback states, missing finish, error callbacks, and invalid callback sequences. `RKLLM_RUN_WAITING` is a documented normal state.

Input must be nonempty valid UTF-8, at most **192 bytes**, with no NUL or non-whitespace ASCII control bytes. This allows approximately 96 Cyrillic letters, sufficient for short room commands, with a separate runtime token-admission check for the 1024-token context. This byte cap is not a token estimate. Startup requires actual bootstrap prefill ≤640, reserves 256 request tokens and 128 response tokens; runtime counts and KV size are checked before any model response is returned. Hitting the output budget fails closed. See Milestone 8 for the RKLLM tokenizer/preflight limitations. The CLI stores at most 193 bytes per line and drains excess bytes before returning U, so the next command remains independent. SafetyGate still receives the unmodified original accepted input.

RKLLM 1.3.0 declares `int rkllm_abort(LLMHandle)` and `int rkllm_is_running(LLMHandle)`. A joined watchdog requests abort after **30 seconds**, including bootstrap; CLI `--timeout-ms N` overrides this. It does not detach inference or destroy the handle while work is running. After a deadline/run/KV/callback fault, the engine remains not-ready and fallback returns U until process restart; the independent fast path remains usable. Invalid user input and malformed model output do not poison an otherwise healthy runtime.

Abort is best effort: the header does not guarantee bounded return time. The caller still waits for synchronous `rkllm_run` and the watchdog to finish. A stuck vendor run/abort can therefore still block shutdown; model initialization and KV clear also have no interruptible deadline. CLI requests taking at least 2000 ms produce a stderr warning. Timing and detailed runtime logs are available with `--verbose`.

## DEV regression on the NPU
```sh
./build/firefly-regression \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --config config/current-m4.json \
  --data tests/data/dev.tsv --verbose \
  > build/dev-regression.tsv 2> build/dev-regression.log
```

This executes the production CommandEngine. The 48-row DEV dataset is balanced between 24 valid actions and 24 expected U responses; it is not a hidden evaluation. Per-case routes/final results and aggregate metrics go to stdout; original input, raw structured output, decode status, typed candidate, safety result, final result and errors remain distinguishable in stderr diagnostics. Exit status is 0 for a clean pass, 1 for regression failures, and 2 for harness/startup errors. See `tests/data/README.md` for metric definitions.

Unit tests need no NPU. Wrapper/CLI tests link stub C functions against the exact SDK header and require Python 3 for subprocess checks; the runtime library is not linked to these tests. A core-only build runs the core and configuration/GenericAction suites without the SDK. No dependency is downloaded automatically.

Production wake gating and XVF3800 acceptance: [V1 Amina wake](docs/v1-amina-wake.md).
Measured Qwen optimization, beep and split command timing: [V1 latency / command UX](docs/v1-qwen-latency-command-ux.md).

Conversational setup, exclusive 1024/4096 model lifecycle, browser voice, and recovery: [dialogue setup](docs/dialogue-setup.md).
