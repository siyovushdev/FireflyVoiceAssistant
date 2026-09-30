# On-board dialogue acceptance

`dialogue_acceptance` is deliberately not part of parallel CTest. It owns one
real RKLLM instance, loads real Admin STT, and creates a private store below
`build/dialogue-recovery/`. It never uses the live user's store or executes hardware.

Build with voice enabled, then run **only when no other RKLLM process is loaded**:

```sh
mkdir -p build/dialogue-recovery
./build/src/admin/dialogue_acceptance build/dialogue-recovery/report.json
```

An optional second argument is a previously confirmed **test** configuration. It
is copied into a fresh private store; this resumes the populated-home scenarios
without repeating the initial empty-home acceptance. No input store is modified.

Run with `Monitor.py` for memory/PSS sampling, admission against loaded RKLLM maps,
1 GiB minimum OS reserve, and graceful cancellation. Never run a build/CTest in
parallel with the NPU acceptance on the 8 GiB board.

The harness records each source message, candidate, questions, latency, inference
metrics, Apply/cancel results, runtime compact reply and recorded DEV STT replay.
It checks exact power-only creation, followup, clarification, the final exclusion
in a multi-step request, model restoration, and `[40]` from the warm runtime model.

After the monitor exits, validate the recorded semantic result as well:

```sh
python3 benchmarks/dialogue-setup/ValidateReport.py build/dialogue-recovery/report.json
```

A completed harness run alone is not the semantic acceptance verdict: the report
validator checks requested names as well as graph sizes, capabilities, final
exclusions and restoration. Rejected runs remain in ignored build artifacts.
The 60-second STT test repeats recorded DEV PCM in memory and uses real Sherpa
while Admin4096 is resident; it is not a physical microphone test.
