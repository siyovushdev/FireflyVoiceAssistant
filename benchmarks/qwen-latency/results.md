# Measured Qwen latency

Means over the identical corpus, real RK3588S/RKLLM 1.3.0. Only actual LLM_FALLBACK rows enter latency/token aggregates. Setup includes context install/bootstrap; startup/model loading is reported separately. No hardware execution.

## baseline

Correctness: 26/26; actual fallback: 26. Startup context installs: 6; total startup/setup: 46871.1 ms.

| Category | n | setup ms | prefill ms | generation ms | total LLM ms | output tokens |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| all | 26 | 2026.65 | 402.55 | 5314.01 | 7760.88 | 20.88 |
| first | 1 | 5292.43 | 419.45 | 5923.54 | 11654.43 | 23.00 |
| same_target | 16 | 0.00 | 405.97 | 5553.34 | 5976.44 | 21.81 |
| target_switch | 9 | 5266.72 | 394.58 | 4820.80 | 10500.61 | 19.00 |

## compact

Correctness: 25/26; actual fallback: 26. Startup context installs: 6; total startup/setup: 46845.7 ms.

| Category | n | setup ms | prefill ms | generation ms | total LLM ms | output tokens |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| all | 26 | 1906.60 | 414.30 | 875.73 | 3213.49 | 3.38 |
| first | 1 | 4997.62 | 404.33 | 1041.40 | 6461.62 | 4.00 |
| same_target | 16 | 0.00 | 419.78 | 890.66 | 1326.50 | 3.44 |
| target_switch | 9 | 4952.68 | 405.66 | 830.78 | 6207.23 | 3.22 |

## final

Correctness: 26/26; actual fallback: 26. Startup context installs: 1; total startup/setup: 21747.4 ms.

| Category | n | setup ms | prefill ms | generation ms | total LLM ms | output tokens |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| all | 26 | 0.00 | 1287.27 | 917.94 | 2220.69 | 3.58 |
| first | 1 | 0.00 | 1185.12 | 1004.84 | 2202.00 | 4.00 |
| same_target | 16 | 0.00 | 1289.14 | 916.91 | 2221.94 | 3.56 |
| target_switch | 9 | 0.00 | 1295.29 | 910.10 | 2220.56 | 3.56 |

## Comparison

| Metric (mean) | JSON baseline | Compact scoped | Final stable | Final improvement vs JSON |
| --- | ---: | ---: | ---: | ---: |
| context_bootstrap_ms | 2026.65 | 1906.60 | 0.00 | 100.0% |
| prefill_ms | 402.55 | 414.30 | 1287.27 | -219.8% |
| generation_ms | 5314.01 | 875.73 | 917.94 | 82.7% |
| total_llm_wall_ms | 7760.88 | 3213.49 | 2220.69 | 71.4% |
| output_tokens | 20.88 | 3.38 | 3.58 | 82.9% |

## Explicit repeated / switch / switch-back requests

| Stage | Transition | Request index | setup ms | prefill ms | generation ms | total LLM ms | output tokens | Correct |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | Kitchen repeat | 1 | 0.00 | 413.01 | 5616.84 | 6048.00 | 22.00 | True |
| baseline | Kitchen → Bedroom | 3 | 5342.09 | 395.38 | 5617.65 | 11374.09 | 22.00 | True |
| baseline | Bedroom repeat | 4 | 0.00 | 422.65 | 5867.48 | 6309.00 | 23.00 | True |
| baseline | Bedroom → Kitchen | 6 | 5235.25 | 409.81 | 5908.13 | 11573.25 | 23.00 | True |
| compact | Kitchen repeat | 1 | 0.00 | 419.29 | 783.58 | 1220.00 | 3.00 | True |
| compact | Kitchen → Bedroom | 3 | 4965.14 | 406.88 | 778.51 | 6169.14 | 3.00 | True |
| compact | Bedroom repeat | 4 | 0.00 | 427.86 | 1037.11 | 1477.00 | 4.00 | True |
| compact | Bedroom → Kitchen | 6 | 4941.45 | 412.89 | 1035.11 | 6407.45 | 4.00 | True |
| final | Kitchen repeat | 1 | 0.00 | 1248.69 | 758.46 | 2024.00 | 3.00 | True |
| final | Kitchen → Bedroom | 3 | 0.00 | 1326.09 | 771.55 | 2115.00 | 3.00 | True |
| final | Bedroom repeat | 4 | 0.00 | 1334.67 | 1042.45 | 2390.00 | 4.00 | True |
| final | Bedroom → Kitchen | 6 | 0.00 | 1192.44 | 1038.21 | 2243.00 | 4.00 | True |
