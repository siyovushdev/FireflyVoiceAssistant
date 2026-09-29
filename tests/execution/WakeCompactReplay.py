#!/usr/bin/env python3
"""One real RKLLM owner, recorded DEV STT, fake wake/ack and existing loopback HTTP mock."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = root / 'build'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', nargs='?', type=Path, default=build / 'wake-compact-replay.json')
parser.add_argument('--original-dev', action='store_true', help='Run the existing eight-recording/Apply harness with explicit HTTP mock bindings')
parser.add_argument('--inline-synthetic', action='store_true', help='Also exercise inline PCM admission, scripted wake-prefixed STT fixtures, real compact Qwen and HTTP mock')
args = parser.parse_args()
output = args.output
with tempfile.TemporaryDirectory(prefix='firefly-wake-replay-') as directory:
    directory = Path(directory)
    ready = directory / 'ready.json'
    with (directory / 'mock.log').open('w+') as log:
        mock = subprocess.Popen([sys.executable, '-u', str(root / 'scripts/v1-mock-device.py'), '--port', '0', '--ready-file', str(ready)], stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if ready.exists():
                    break
                if mock.poll() is not None:
                    raise RuntimeError('HTTP mock exited')
                time.sleep(.02)
            port = json.loads(ready.read_text())['port']
            config = json.loads((root / 'config/demo/v1-kitchen.json').read_text())
            config['bindings'][0]['http']['url'] = f'http://127.0.0.1:{port}/brightness'
            if args.original_dev:
                config['bindings'] = []
                for device in config['devices']:
                    for action in device['action_ids']:
                        http = {'method': 'POST', 'url': f'http://127.0.0.1:{port}/brightness', 'timeout_ms': 1500, 'body': {}}
                        if action == 'action_set_brightness':
                            http['argument'] = 'brightness'
                        config['bindings'].append({'target_id': device['id'], 'action_id': action, 'executor': 'http', 'http': http})
            path = directory / 'configuration.json'
            path.write_text(json.dumps(config, ensure_ascii=False))
            command = [str(build / ('unified_speech_replay_tests' if args.original_dev else 'wake_compact_replay')),
                       str(build / 'stt/model'), str(build / 'stt/vad-ab/silero_vad.onnx'), str(build / 'stt'),
                       '/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm', str(path)]
            if not args.original_dev:
                command.append(str(output))
                if args.inline_synthetic:
                    command.append('--inline-synthetic')
            subprocess.run(command, check=True)
            log.flush(); log.seek(0)
            requests = [line for line in log.read().splitlines() if line.startswith('v1-mock-device: /brightness ')]
            expected = (None, 40, 50, 0, 40) if args.original_dev else (40, 100, 0)
            if args.inline_synthetic and not args.original_dev:
                expected += (40, 100, 0)
            assert len(requests) == len(expected), requests
            for value, line in zip(expected, requests):
                assert ('{}' if value is None else f"'brightness': {value}") in line, line
            if not args.original_dev:
                report = json.loads(output.read_text())
                report['http_mock_requests'] = requests
                output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
            print('PASS: real DEV STT/Qwen, explicit HTTP mock request counts, safe rejections')
        finally:
            mock.terminate()
            mock.wait(timeout=5)
