# DEV text regression

`dev.tsv` is development/regression data, not HIDDEN evaluation. Each non-comment row has four tab-separated fields: stable id, category, expected compact result, original Russian text. Preserve input spacing and punctuation. Expected results must be U or strict compact actions. Keep valid and unsupported categories balanced when adding cases. Never alter expected results just to match model output.

The runner loads the model once and invokes production CommandEngine for every row. It deliberately includes repeated commands to check independence. No reference file is written.

## Metrics
- `valid_correct`: expected action exactly matched (including D).
- `unsupported_correct`: expected U, returned U.
- `false_accept`: expected U, returned an action.
- `false_reject`: expected action, returned U.
- `wrong_action`: expected action, returned a different action.
- `invalid_output`: malformed normalized **model output**, excluding U, before SafetyGate; diagnostic and may overlap a correct final U or a false rejection.
- `final_invalid_output`: malformed final output; must remain zero.
- `runtime_or_input_errors`: requests carrying a runtime/input failure diagnostic.
- `fast_path`, `llm_fallback`, `input_rejected`: exclusive routing counts.
- Latency includes input checks, parsing, KV clear/inference when applicable, gate, and validation; excludes model initialization/bootstrap. First fast request includes one-time regex compilation.

Raw and normalized LLM output are recorded separately from final output in stderr diagnostics; blank raw output on fast-path rows means the LLM was bypassed. stdout contains per-case outcomes and a summary. All unexpected outcomes are reported without tuning the frozen prompt.
