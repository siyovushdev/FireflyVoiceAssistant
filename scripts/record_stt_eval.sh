#!/usr/bin/env bash
# Evaluation capture only. Does not invoke STT, the assistant, or hardware actions.
set -euo pipefail

if [[ ${1:-} == --help ]]; then
    cat <<'HELP'
Usage: bash scripts/record_stt_eval.sh

Interactively record eight Russian phrases with arecord:
  device: plughw:CARD=H340,DEV=0
  format: WAV, mono, 16000 Hz, S16_LE
  length: 8 seconds per phrase, following a 3-second countdown
  output: build/stt/eval/ relative to the repository

Press Enter when ready for each phrase. After recording, keep or retry.
Existing recordings require confirmation before replacement.
Run from a local terminal on the Firefly device. Ctrl+C stops recording.
HELP
    exit 0
fi
if (( $# != 0 )); then
    printf 'Unexpected argument. Use --help for instructions.\n' >&2
    exit 2
fi
command -v arecord >/dev/null || { printf 'arecord is required.\n' >&2; exit 1; }
[[ -t 0 ]] || { printf 'Run this helper in an interactive terminal.\n' >&2; exit 1; }

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output_dir="$script_dir/../build/stt/eval"
mkdir -p -- "$output_dir"
device='plughw:CARD=H340,DEV=0'
files=(01_door.wav 02_open_door.wav 03_open_entrance_door.wav 04_light_on.wav
       05_light_off.wav 06_kitchen_40.wav 07_bedroom_half.wav 08_corridor_off.wav)
phrases=('дверь' 'открой дверь' 'открой входную дверь' 'включи свет'
         'выключи свет' 'на кухне поставь свет на сорок процентов'
         'в спальне свет наполовину' 'в коридоре выключи свет')
partial=''
cleanup() {
    if [[ -n $partial && -f $partial ]]; then rm -f -- "$partial"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'Device: %s\nOutput: %s\n' "$device" "$output_dir"
printf 'Speak the displayed phrase once, naturally, during each 8-second recording.\n'
printf 'No speech synthesis, STT, mixer changes, or assistant execution will occur.\n'
for i in "${!files[@]}"; do
    destination="$output_dir/${files[i]}"
    printf '\n[%d/8] %s\nSay: «%s»\n' "$((i + 1))" "${files[i]}" "${phrases[i]}"
    if [[ -e $destination ]]; then
        read -r -p 'Already exists. Type replace to re-record, or Enter to keep: ' reply
        if [[ $reply != replace ]]; then continue; fi
    fi
    while true; do
        read -r -p 'Press Enter when ready (or type q to stop): ' reply
        if [[ $reply == q ]]; then exit 0; fi
        for remaining in 3 2 1; do printf '%s...\n' "$remaining"; sleep 1; done
        partial=$(mktemp "$output_dir/.recording-XXXXXX.wav")
        printf 'RECORDING — speak now: «%s»\n' "${phrases[i]}"
        if ! arecord --device="$device" --file-type=wav --format=S16_LE \
                --rate=16000 --channels=1 --duration=8 --fatal-errors "$partial"; then
            printf 'Capture failed. The previous recording, if any, was preserved.\n' >&2
            exit 1
        fi
        printf 'Recording finished.\n'
        while true; do
            read -r -p 'Enter to keep, r to retry, q to discard and stop: ' reply
            case "$reply" in
                '') mv -f -- "$partial" "$destination"; partial=''; break ;;
                r) cleanup; partial=''; break ;;
                q) exit 0 ;;
                *) printf 'Please choose Enter, r, or q.\n' ;;
            esac
        done
        if [[ $reply != r ]]; then break; fi
    done
done
printf '\nAll eight recording steps completed. Files are in %s\n' "$output_dir"
printf 'Tell the assistant the recordings are ready for WAV-only evaluation.\n'
