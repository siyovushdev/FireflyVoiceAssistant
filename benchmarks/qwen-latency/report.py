#!/usr/bin/env python3
"""Regenerate CSV and measured aggregate tables from immutable stage JSON."""
import csv
import json
from pathlib import Path
import statistics

root = Path(__file__).resolve().parent
metrics = ['context_bootstrap_ms', 'prefill_ms', 'generation_ms', 'total_llm_wall_ms', 'output_tokens']
reports = {}
for name in ('baseline', 'compact', 'final'):
    path = root / (name + '.json')
    if not path.exists():
        continue
    value = json.loads(path.read_text())
    rows = value['requests']
    with (root / (name + '.csv')).open('w', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows({k: json.dumps(v, ensure_ascii=False) if isinstance(v, (dict, list)) else v for k, v in row.items()} for row in rows)
    reports[name] = value
lines = ['# Measured Qwen latency', '', 'Means over the identical corpus, real RK3588S/RKLLM 1.3.0. Only actual LLM_FALLBACK rows enter latency/token aggregates. Setup includes context install/bootstrap; startup/model loading is reported separately. No hardware execution.', '']
for name, value in reports.items():
    rows = value['requests']
    fallback = [r for r in rows if r['fallback']]
    lines += [f'## {name}', '', f'Correctness: {sum(r["correct"] for r in rows)}/{len(rows)}; actual fallback: {len(fallback)}. Startup context installs: {value["startup_context_installs"]}; total startup/setup: {value["startup_setup_ms"]:.1f} ms.', '', '| Category | n | setup ms | prefill ms | generation ms | total LLM ms | output tokens |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for category in ('all', 'first', 'same_target', 'target_switch'):
        selected = [r for r in fallback if category == 'all' or r['category'] == category]
        if selected:
            lines.append(f'| {category} | {len(selected)} | ' + ' | '.join(f'{statistics.mean(r[m] for r in selected):.2f}' for m in metrics) + ' |')
    lines += ['']
if len(reports) == 3:
    lines += ['## Comparison', '', '| Metric (mean) | JSON baseline | Compact scoped | Final stable | Final improvement vs JSON |', '| --- | ---: | ---: | ---: | ---: |']
    for metric in metrics:
        values = [statistics.mean(r[metric] for r in reports[n]['requests'] if r['fallback']) for n in ('baseline', 'compact', 'final')]
        lines.append('| ' + metric + ' | ' + ' | '.join(f'{v:.2f}' for v in values) + f' | {(1-values[2]/values[0])*100:.1f}% |')
if len(reports) == 3:
    lines += ['', '## Explicit repeated / switch / switch-back requests', '',
              '| Stage | Transition | Request index | setup ms | prefill ms | generation ms | total LLM ms | output tokens | Correct |',
              '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |']
    for name, report in reports.items():
        for index, label in ((1, 'Kitchen repeat'), (3, 'Kitchen → Bedroom'), (4, 'Bedroom repeat'), (6, 'Bedroom → Kitchen')):
            row = report['requests'][index]
            lines.append(f'| {name} | {label} | {index} | ' + ' | '.join(f'{row[m]:.2f}' for m in metrics) + f' | {row["correct"]} |')
(root / 'results.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
