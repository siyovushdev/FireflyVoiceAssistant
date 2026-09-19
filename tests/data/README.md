# DEV text regression

`dev.tsv` is development/regression data, not HIDDEN evaluation. Each non-comment row has four tab-separated fields: stable id, category, expected compact result, original Russian text. Preserve input spacing and punctuation. Expected results must be U or strict compact actions. Keep valid and unsupported categories balanced when adding cases. Never alter expected results just to match model output.

The runner loads the model once and invokes production CommandEngine for every row. It deliberately includes repeated commands to check independence. No reference file is written.

## Metrics
- `valid_correct`: expected action exactly matched (including D).
- `unsupported_correct`: expected U, returned U.
- `false_accept`: expected U, returned an action.
- `false_reject`: expected action, returned U.
- `wrong_action`: expected action, returned a different action.
- `invalid_output`: structured decoder rejection before SafetyGate (malformed JSON, invalid shape/IDs/arguments/domain); diagnostic and may overlap a correct final U or a false rejection.
- `final_invalid_output`: malformed final output; must remain zero.
- `runtime_or_input_errors`: requests carrying a runtime/input failure diagnostic.
- `fast_path`, `llm_fallback`, `input_rejected`: exclusive routing counts.
- Latency includes input checks, parsing, KV clear/inference when applicable, gate, and validation; excludes model initialization/bootstrap. First fast request includes one-time regex compilation.

Raw structured output, decode status, semantic candidate and SafetyGate result are recorded separately from final output in stderr diagnostics; blank raw output on fast-path rows means the LLM was bypassed. stdout contains per-case outcomes and a summary. All unexpected outcomes are reported. DEV labels remain frozen; Milestone 8 generates the production prompt from configuration.

Structured diagnostics: `structured_actions` and `structured_unsupported` count accepted decoder results on RKLLM fallback; `malformed_json` counts invalid JSON/duplicate keys/resource rejection; `invalid_structured` counts valid JSON rejected by shape/domain checks. `decoder_rejects` is their sum. `safety_rejects` counts executable candidates rejected by the deterministic gate; `domain_rejects` counts shared final-domain rejections. `llm_semantic_wrong` compares accepted decoder candidates (before SafetyGate) with DEV semantics, including incorrect Unsupported results; fast-path rows are excluded. These are candidate-quality metrics, separate from final-action acceptance.
