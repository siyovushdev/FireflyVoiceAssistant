"""Production contract, without systemd install, NPU, STT models or microphone.

ExecStart is deliberately direct. Compare its parsed argv with the *executed*
helper using a private fake executable, rather than grepping shell source.
"""
import configparser
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

BOARD = '/home/firefly/FireflyVoiceAssistantLinux'
MODEL_ROOT = '/home/firefly/demo_Linux_aarch64'
REQUIRED = {
    '--store': '/var/lib/firefly/store',
    '--init-config': BOARD + '/config/empty-home.json',
    '--model': MODEL_ROOT + '/Qwen3-4B_W8A8_RK3588_3core.rkllm',
    '--admin-bind': '127.0.0.1', '--admin-port': '8080',
    '--voice': True,
    '--stt-model-dir': BOARD + '/build/stt/model',
    '--vad-model': BOARD + '/build/stt/vad-ab/silero_vad.onnx',
    '--audio-device': 'plughw:CARD=Array,DEV=0',
    '--wake-enabled': 'true',
    '--wake-python': BOARD + '/build/wake-venv/bin/python',
    '--wake-model': BOARD + '/assets/wake/amina-v3.tflite',
    '--wake-cutoff': '0.99', '--wake-window': '7',
    '--playback-device': 'plughw:CARD=rockchipes8388,DEV=0',
}


def unit_at(root):
    unit = configparser.ConfigParser(interpolation=None)
    unit.optionxform = str
    text = (root / 'packaging/firefly-voice-assistant.service').read_text()
    unit.read_string(re.sub(r'\\\n\s*', ' ', text))
    return unit


def arguments(argv):
    result = {}
    tokens = iter(argv)
    for flag in tokens:
        assert flag.startswith('--') and flag not in result, ('invalid/duplicate flag', flag)
        result[flag] = True if flag == '--voice' else next(tokens)
    return result


def assert_contract(options):
    assert options == REQUIRED, (options, REQUIRED)


class ProductionContract(unittest.TestCase):
    def test_unit_arguments_and_supervision(self):
        unit = unit_at(ROOT)
        service = unit['Service']
        command = shlex.split(service['ExecStart'])
        self.assertEqual(command[0], BOARD + '/build/firefly-demo')
        assert_contract(arguments(command[1:]))
        for key, value in {
            'Type': 'simple', 'User': 'firefly', 'Group': 'firefly',
            'WorkingDirectory': BOARD, 'Restart': 'on-failure', 'RestartSec': '5',
            'TimeoutStopSec': '45', 'KillSignal': 'SIGTERM', 'KillMode': 'mixed',
            'NoNewPrivileges': 'true', 'PrivateTmp': 'true', 'ProtectSystem': 'strict',
            'ProtectHome': 'read-only', 'ReadWritePaths': '/var/lib/firefly/store',
            'StandardOutput': 'journal', 'StandardError': 'journal',
        }.items():
            self.assertEqual(service[key], value, key)
        self.assertEqual(shlex.split(service['Environment']), ['LD_LIBRARY_PATH=' + MODEL_ROOT + '/lib'])
        self.assertEqual(unit['Install']['WantedBy'], 'multi-user.target')
        self.assertEqual(unit['Unit']['StartLimitIntervalSec'], '120')
        self.assertEqual(unit['Unit']['StartLimitBurst'], '5')
        self.assertNotIn('ExecStartPre', service)
        self.assertNotIn('ExecStop', service)
        # --voice used to be absent. Every mandatory flag is a failing mutation.
        for flag in REQUIRED:
            missing = dict(REQUIRED)
            del missing[flag]
            with self.assertRaises(AssertionError, msg=flag):
                assert_contract(missing)
        for flag, value in [('--admin-bind', '0.0.0.0'), ('--wake-enabled', 'false'),
                            ('--init-config', BOARD + '/config/current-m4.json')]:
            wrong = {**REQUIRED, flag: value}
            with self.assertRaises(AssertionError):
                assert_contract(wrong)

    def test_helper_default_argv_matches_direct_unit(self):
        with tempfile.TemporaryDirectory(prefix='firefly-startup-contract-') as directory:
            root = Path(directory)
            for name in ['scripts', 'build/stt/model', 'build/stt/vad-ab', 'build/wake-venv/bin', 'bin', 'store']:
                (root / name).mkdir(parents=True, exist_ok=True)
            helper = root / 'scripts/firefly-dialogue.sh'
            shutil.copyfile(ROOT / 'scripts/firefly-dialogue.sh', helper)
            (root / 'build/stt/vad-ab/silero_vad.onnx').touch()
            for name, content in {
                # Only replaces helper's read-only /proc competing-runtime guard.
                'bin/python3': '#!/bin/sh\nexit 0\n',
                'build/wake-venv/bin/python': '#!/bin/sh\nexit 0\n',
                'build/firefly-demo': '#!' + sys.executable + '\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n',
            }.items():
                path = root / name
                path.write_text(content)
                path.chmod(0o700)
            env = {k: v for k, v in os.environ.items() if not k.startswith('FIREFLY_')}
            env['PATH'] = str(root / 'bin') + os.pathsep + env['PATH']
            def run(*extra):
                return subprocess.run(['bash', str(helper), str(root / 'store'), *extra],
                                      env=env, text=True, capture_output=True, timeout=10)
            absent = run()
            self.assertEqual(absent.returncode, 2, absent.stderr)
            for existing in [False, True]:
                if existing:
                    (root / 'store/configuration.json').write_text('existing sentinel')
                result = run('--new')
                self.assertEqual(result.returncode, 0, result.stderr)
                options = arguments(json.loads(result.stdout))
                options = {k: v.replace(str(root), BOARD) if isinstance(v, str) else v for k, v in options.items()}
                options['--store'] = REQUIRED['--store']  # helper deliberately accepts caller's private store
                assert_contract(options)
            result = run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('--init-config', arguments(json.loads(result.stdout)))
            self.assertEqual((root / 'store/configuration.json').read_text(), 'existing sentinel')

    def test_production_seed_has_no_demo_entities(self):
        seed = json.loads((ROOT / 'config/empty-home.json').read_text())
        self.assertEqual(seed['schema_version'], 2)
        for name in ['rooms', 'devices', 'actions', 'bindings', 'connections', 'observations']:
            self.assertEqual(seed.get(name, []), [], name)


if __name__ == '__main__':
    ROOT = Path(sys.argv.pop(1)).resolve()
    unittest.main()
