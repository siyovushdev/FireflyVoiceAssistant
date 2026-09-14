#!/usr/bin/env python3
"""Fresh human H340 DEV recording only; no recognition or command processing."""
import argparse
import array
import datetime
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time
import wave

REPO = Path(__file__).resolve().parents[3]
RECORDINGS = REPO / "build/stt-wake-55/recordings"
DEVICE = "plughw:CARD=H340,DEV=0"


def plan():
    cases = []
    def add(prompt, **metadata):
        cases.append(dict(id=f"dev_{len(cases) + 1:02d}", prompt=prompt, duration_s=10, **metadata))
    for distance in (5, 50):
        for form in ("файрфлай", "файерфлай"):
            for suffix, pause, repetition in (
                ("", "na", 1), ("", "na", 2),
                ("включи свет на кухне", "none", 1),
                ("в спальне поставь свет на семьдесят процентов", "none", 1),
                ("открой входную дверь", "short", 1),
                ("какая сегодня погода", "short", 1),
            ):
                add(form.capitalize() + (" " + suffix if suffix else ""), expected_wake=True,
                    spoken_form=form, expected_suffix=suffix, distance_cm=distance, pause=pause,
                    mode="one_utterance" if suffix else "wake_only", repetition=repetition)
    for prompt, category in (
        ("Включи свет на кухне", "command_without_wake"),
        ("В спальне поставь свет на семьдесят процентов", "command_without_wake"),
        ("Открой входную дверь", "command_without_wake"),
        ("Какая сегодня погода", "command_without_wake"),
        ("Файл открыт, флаг поднят, фарфор стоит на полке", "similar"),
        ("Фейерверк закончился, отправь файл Андрею", "similar"),
        ("Я сказал файрфлай включи свет на кухне", "wake_later_in_sentence"),
        ("Мы обсуждали название файерфлай после обеда", "wake_later_in_sentence"),
        ("Сегодня хорошая погода, после обеда я пойду гулять", "conversation_short"),
        ("[Молчите 60 секунд; оставьте обычный тихий фон комнаты]", "silence"),
        ("[30 секунд без речи: пошуршите бумагой, поработайте клавиатурой, подвигайте стул]", "room_noise"),
        ("[3 минуты обычной русской речи о вашем дне, еде и планах; не произносите название продукта]", "conversation_long"),
    ):
        add(prompt, expected_wake=False, spoken_form="", expected_suffix="", distance_cm=5,
            pause="none", mode="negative", category=category)
        cases[-1]["duration_s"] = {"silence": 60, "room_noise": 30, "conversation_long": 180}.get(category, 10)
    return cases


def stats(path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise ValueError("Unexpected capture format")
        samples = array.array("h", wav.readframes(wav.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    power = sum(x * x for x in samples) / len(samples) if samples else 0
    return dict(recorded_duration_s=len(samples) / 16000, peak_pcm=max(map(abs, samples), default=0),
        clipped_samples=sum(abs(x) >= 32767 for x in samples),
        rms_dbfs=10 * math.log10(power / 32768 ** 2) if power else None,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def describe(case):
    pause = {"none": "без намеренной паузы", "short": "короткая естественная пауза после имени (~0,2–0,3 с)",
             "na": "только имя; ничего не добавляйте"}[case["pause"]]
    return f"{case['id']} — {case['distance_cm']} см; {pause}; {case['duration_s']} с\n  «{case['prompt']}»"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="Show exact prompts without opening the microphone")
    parser.add_argument("--resume", type=Path, help="Resume the displayed session directory")
    args = parser.parse_args()
    cases = plan()
    if args.list:
        print("\n".join(describe(row) for row in cases))
        return
    if not sys.stdin.isatty() or not shutil.which("arecord"):
        parser.error("Run in a local interactive terminal on Firefly with arecord available")
    if args.resume:
        session = args.resume.resolve()
        if session.parent != RECORDINGS.resolve():
            parser.error("Resume must name a session under build/stt-wake-55/recordings")
        manifest = json.loads((session / "manifest.json").read_text())
        if manifest["split"] != "fresh_dev" or manifest["planned_cases"] != cases:
            parser.error("Session is not this DEV recording plan")
    else:
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        session = RECORDINGS / ("dev-" + stamp)
        session.mkdir(parents=True, exist_ok=False)
        manifest = dict(split="fresh_dev", device=DEVICE, format="16000 Hz mono S16_LE",
            created_utc=stamp, accepted_policy=None, planned_cases=cases, cases=[], recording_complete=False)

    def save():
        temporary = session / "manifest.tmp"
        temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        temporary.replace(session / "manifest.json")

    save()
    print(f"Session: {session}\nResume: python3 experiments/stt-wake-55/scripts/record.py --resume {session}")
    print("DEV only. No recognizer runs. Both spellings remain provisional. All attempts are retained.")
    print("Measure from mouth to H340 microphone: approximately 5 cm, then 50 cm. Keep its orientation stable.")
    print("After SPEAK NOW say the phrase ONCE, naturally, then stay quiet. Finish short phrases within 6 seconds.")
    print("Retry only a capture/speaking mistake; record the reason. Do not optimize pronunciation for recognition.")
    print("For later-name negatives, say the whole sentence continuously without pausing before the name.")
    try:
        for case in cases:
            if any(r["id"] == case["id"] and r.get("selected") for r in manifest["cases"]):
                continue
            while True:
                print("\n" + describe(case))
                if input("Enter when positioned and ready; q to stop: ").strip().lower() == "q":
                    return
                attempt = 1 + sum(r["id"] == case["id"] for r in manifest["cases"])
                path = session / f"{case['id']}-attempt{attempt}.wav"
                if path.exists():
                    raise RuntimeError("Refusing to overwrite recording")
                row = dict(**case, file=str(path), attempt=attempt, selected=False, status="capture_started")
                manifest["cases"].append(row)
                save()
                for value in (3, 2, 1):
                    print(f"{value}...", flush=True)
                    time.sleep(1)
                command = ["arecord", "--device=" + DEVICE, "--file-type=wav", "--format=S16_LE",
                    "--rate=16000", "--channels=1", "--duration=" + str(case["duration_s"]),
                    "--fatal-errors", str(path)]
                start = time.monotonic()
                with subprocess.Popen(command) as process:
                    print("RECORDING — stay quiet for one second...", flush=True)
                    time.sleep(1)
                    if process.poll() not in (None, 0):
                        raise RuntimeError("H340 capture failed")
                    print("SPEAK NOW / begin the displayed activity: " + case["prompt"], flush=True)
                    row["arecord_exit"] = process.wait()
                row["capture_wall_s"] = time.monotonic() - start
                row["status"] = "capture_finished"
                if row["arecord_exit"]:
                    raise RuntimeError("Capture failed; recording preserved")
                row.update(stats(path))
                if abs(row["recorded_duration_s"] - case["duration_s"]) > .05:
                    raise RuntimeError("Incomplete capture; recording preserved")
                save()
                while True:
                    answer = input("Enter to keep; r for a speaking/capture mistake; q to stop: ").strip().lower()
                    if answer in ("", "r", "q"):
                        break
                row["selected"] = answer == ""
                if answer == "r":
                    row["retry_reason"] = input("Describe the mistake (all audio is retained): ")
                elif answer == "":
                    row["speaker_note"] = input("Actual distance or wording deviation, if any; otherwise Enter: ")
                save()
                if answer == "q":
                    return
                if row["selected"]:
                    break
        manifest["recording_complete"] = True
    finally:
        save()
        print(f"\nSaved: {session}\nSend this path to the assistant. Validation is a separate later recording session.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped; attempts retained.", file=sys.stderr)
        raise SystemExit(130)
