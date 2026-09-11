# Repository Guidelines

## Fixed Target and Scope
FireflyVoiceAssistantLinux targets Linux/aarch64 only: Firefly ROC-RK3588S-PC, Ubuntu 22.04, RK3588S, 8 GB RAM. The fixed inference stack is RKLLM 1.3.0 with Qwen3-4B W8A8 on the NPU. Do not select another LLM, change quantization, rebuild the model, redo Vulkan/NPU research, or work on Android.

The current milestone is TEXT ONLY: no microphone, STT, GPIO/hardware execution, or systemd. Do not integrate RKLLM during the first implementation milestone. Never use serialized persistent prompt cache as production state.

## Read-Only Production References
`~/demo_Linux_aarch64` is strictly READ-ONLY. Never modify, rename, delete, or overwrite anything there. Inspect only necessary listed references:
- `rkllm.h`
- `Qwen3-4B_W8A8_RK3588_3core.rkllm`
- `prompt_compact_v5_short_system.txt`
- `firefly_benchmark_runner_memory_system_v1.cpp`
- `firefly_benchmark_runner_safety_v1.cpp`

Do not tune prompt v5. Do not replace the compact protocol with JSON.

## Frozen Protocol and Architecture
Pipeline: original text → conservative deterministic parser → RKLLM/Qwen3-4B if unresolved (future integration) → SafetyGate → strict ActionValidator → Action.

Protocol: `L|K|N` kitchen, `L|B|N` bedroom, `L|G|N` living room/hall, `L|C|N` corridor; integer N is 0..100. `D` means open entrance door. `U` means unsupported, ambiguous, or unsafe and is never executable.

The parser must never guess. Exact commands require explicit room and brightness; on/maximum = 100, off = 0, half = 50. SafetyGate may only preserve the candidate or reject to U, never repair or invent. For light actions, the predicted room must occur explicitly in ORIGINAL input. Reject brightness 0 when original input contains a form of «включить». Strict validation accepts only D or the four valid light forms.

## Implementation and Verification
Use maintainable C++17, CMake, separated responsibilities, RAII where appropriate, and no unnecessary global state. Public interfaces live in `include/firefly/`, implementations in `src/`, unit tests in `tests/`. Use four-space indentation and descriptive names.

First milestone: inspect repository and necessary references; implement Action, ActionValidator, DeterministicParser, SafetyGate; test all four without RKLLM. Run:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Stop after tests pass. Report files created, architecture, build command, tests/results, and proposed next step. For future changes, add regression tests for safety failures. Keep commits focused; PRs describe behavior and validation. Never commit model binaries or credentials.
