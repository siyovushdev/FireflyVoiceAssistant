#!/usr/bin/env python3
"""Replay DEV/legacy WAVs through unchanged VAD/STT; inspect each exact prefix separately."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess

from prefix import PROVISIONAL_FORMS, NORMALIZATION, extract, normalize

REPO = Path(__file__).resolve().parents[3]


def sha256(path):
    with Path(path).open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def percentage(hits, total):
    return dict(hits=hits, total=total, percent=100 * hits / total if total else None)


def metrics(rows):
    positives = [r for r in rows if r["expected_wake"]]
    negatives = [r for r in rows if not r["expected_wake"]]
    no_pause = [r for r in positives if r.get("pause") == "none" and r.get("mode") == "one_utterance"]
    return dict(
        recall=percentage(sum(r["wake_detected"] for r in positives), len(positives)),
        false_wake_samples=sum(r["wake_detected"] for r in negatives),
        negative_samples=len(negatives),
        false_wake_events=sum(r["wake_events"] for r in negatives),
        negative_audio_s=sum(r["performance"]["audio_s"] for r in negatives),
        extra_positive_wake_events=sum(max(0, r["wake_events"] - 1) for r in positives),
        no_pause_exact_wake_and_suffix=percentage(sum(r["exact_mode_success"] for r in no_pause), len(no_pause)),
        exact_mode_success=percentage(sum(r["exact_mode_success"] for r in positives), len(positives)),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--paced", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    # Fresh validation is deliberately unavailable until a DEV decision is locked.
    if manifest.get("split") not in ("archived_dev", "fresh_dev", "legacy_negative_audit"):
        parser.error("Only DEV or explicitly labelled legacy negatives are enabled before the DEV lock")
    if not manifest.get("recording_complete", True):
        parser.error("Recording session is incomplete; review completion before scoring")
    cases = [r for r in manifest["cases"] if r.get("selected", True)]
    if not cases or len({r["id"] for r in cases}) != len(cases):
        parser.error("Empty manifest or duplicate case ids")
    for row in cases:
        if not isinstance(row["expected_wake"], bool):
            parser.error("expected_wake must be boolean")
        row["file"] = str(Path(row["file"]).resolve())
        if sha256(row["file"]) != row["sha256"]:
            parser.error("WAV hash mismatch: " + row["file"])
    if len({r["file"] for r in cases}) != len(cases):
        parser.error("Duplicate WAV path")
    args.output.mkdir(parents=True, exist_ok=False)
    write_json(args.output / "manifest.json", manifest)
    binary = REPO / "build/stt-wake-55/bin/stt-wake-replay"
    command = [str(binary), str(REPO / "build/stt/model"),
               str(REPO / "build/stt/vad-ab/silero_vad.onnx"),
               "--paced" if args.paced else "--unpaced", *[r["file"] for r in cases]]
    write_json(args.output / "run.json", dict(command=command, binary_sha256=sha256(binary),
        manifest_sha256=sha256(args.manifest), platform=platform.platform(),
        normalization=NORMALIZATION, provisional_forms=PROVISIONAL_FORMS, accepted_policy=None,
        settings="unchanged Milestone 4 SherpaSpeech.cpp and VoiceSegmenter.cpp; 300/300 ms",
        source_hashes={name: sha256(REPO / name) for name in (
            "src/SherpaSpeech.cpp", "src/VoiceSegmenter.cpp",
            "experiments/stt-wake-55/src/SttWakeReplay.cpp", "experiments/stt-wake-55/scripts/prefix.py")}))
    with (args.output / "raw.jsonl").open("w") as stdout, (args.output / "stderr.txt").open("w") as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
    if result.returncode:
        raise SystemExit(f"Replay failed ({result.returncode}); partial raw logs preserved at {args.output}")
    output = [json.loads(line) for line in (args.output / "raw.jsonl").read_text().splitlines()]
    summaries = {r["file"]: r for r in output if r["type"] == "sample"}
    events = collections.defaultdict(list)
    for row in output:
        if row["type"] == "utterance":
            events[row["file"]].append(row)
    scored = []
    for case in cases:
        sample_events = events[case["file"]]
        performance = summaries[case["file"]]
        for form in PROVISIONAL_FORMS:
            details = []
            for event in sample_events:
                detection = extract(event["raw_transcript"], form) if event["success"] else extract("", form)
                details.append(dict(**event, **detection))
            detected = [r for r in details if r["wake_detected"]]
            exact_mode = (len(details) == 1 and len(detected) == 1 and not performance["active_at_eof"]
                          and normalize(case.get("expected_suffix", "")) == detected[0]["command_suffix"])
            scored.append(dict(**case, candidate_form=form, performance=performance,
                wake_detected=bool(detected), wake_events=len(detected), events=details,
                exact_mode_success=bool(exact_mode)))
    summary = dict(split=manifest["split"], final_validation=False, accepted_policy=None,
        initialization=output[0], candidates={})
    for form in PROVISIONAL_FORMS:
        rows = [r for r in scored if r["candidate_form"] == form]
        groups = {}
        for key in ("spoken_form", "mode", "pause", "distance_cm"):
            values = sorted({str(r.get(key, "unknown")) for r in rows})
            groups[key] = {v: metrics([r for r in rows if str(r.get(key, "unknown")) == v]) for v in values}
        summary["candidates"][form] = dict(**metrics(rows), groups=groups)
    positives = [r for r in cases if r["expected_wake"]]
    exact_word = sum(bool(events[r["file"]]) and events[r["file"]][0]["success"]
                     and extract(events[r["file"]][0]["raw_transcript"], r["spoken_form"])["wake_detected"]
                     for r in positives)
    summary["exact_spoken_wake_at_first_transcript_prefix"] = percentage(exact_word, len(positives))
    summary["positive_first_transcripts"] = dict(collections.Counter(
        events[r["file"]][0]["raw_transcript"] if events[r["file"]] else "[NO STT]" for r in positives))
    latencies = [r["stt_ms"] for r in output if r["type"] == "utterance" and r["padded_s"] > 0]
    summary["average_stt_ms"] = statistics.mean(latencies) if latencies else None
    summary["stt_latency_count"] = len(latencies)
    summary["errors"] = sum(r["errors"] for r in summaries.values())
    summary["active_eof_samples"] = sum(r["active_at_eof"] for r in summaries.values())
    summary["process_peak_rss_kib_cumulative"] = max(r["process_peak_rss_kib_cumulative"] for r in summaries.values())
    write_json(args.output / "samples.json", scored)
    write_json(args.output / "summary.json", summary)

    def cell(value):
        return str(value).replace("|", "\\|").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")

    lines = ["# Full-STT wake prefix observations", "", f"Split: {manifest['split']}. No accepted policy selected.", "",
             "Both provisional forms are independent probes. JSON preserves exact raw text and all segment errors.", "",
             "| Sample/event | Candidate | Expected wake | Raw STT | Matched prefix | Wake | Command suffix | STT ms |",
             "|---|---|---|---|---|---|---|---:|"]
    for row in scored:
        details = row["events"] or [dict(event="none", raw_transcript="[STT SKIPPED]", accepted_prefix=None,
                                         wake_detected=False, command_suffix=None, stt_ms=None)]
        for event in details:
            values = [row["id"] + "/" + str(event["event"]), row["candidate_form"], row["expected_wake"],
                      event["raw_transcript"], event["accepted_prefix"], event["wake_detected"],
                      event["command_suffix"], event["stt_ms"]]
            lines.append("| " + " | ".join(cell(v) for v in values) + " |")
    lines += ["", "Performance is in samples.json. VAD compute time differs from endpoint delay. Peak RSS is",
              "process cumulative; initialization is separate. Unpaced CPU/wall is not live utilization.",
              "Paced replay is synchronous with catch-up and reports delivery lag; it is not production capture.",
              "No second-utterance state, command processing or action scoring is implemented."]
    (args.output / "REPORT.md").write_text("\n".join(lines) + "\n")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
