# Rejected protocol trials (DEV only)

These exploratory reports preserve raw outputs and end-to-end safety decisions.
They are not the final before/after performance tables. All used the same 26 inputs,
same Qwen model/sampling, unchanged SafetyGate, and no hardware execution.

| File | Output contract | Context | Correct | Incorrect executable actions |
| --- | --- | --- | ---: | ---: |
| bare-scoped.json | bare integer / A / U | scoped | 19/26 | 0 |
| final-attempt1.json | bare integer / A / U; numeral glossary | stable | 23/26 | 0 |
| final-attempt2.json | bare integer / A / U; explicit output metadata | stable | 21/26 | 0 |
| array-attempt.json | [integer] / A / U | stable | 20/26 | 0 |
| array-null-attempt.json | [integer] / [] / null | stable | 26/26 | 0 |

All incorrect final results were Unsupported, including model-produced wrong scalar
values rejected by the unchanged safety checks. The last run overlapped compilation;
its latency is **not** used for the final performance comparison. The accepted
protocol was subsequently measured serially with the board free of build/test load.
See the parent directory's scoped compact and final stable reports.
