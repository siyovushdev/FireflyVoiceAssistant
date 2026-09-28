"""Model-backed CPU tests. No microphone, NPU, commands, or hardware."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import numpy as np

root = Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location('amina_worker', root / 'runtime/wake/amina_worker.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
model = root / 'assets/wake/amina-v3.tflite'


class Tests(unittest.TestCase):
    def test_model_state_and_bounded_silence(self):
        engine = module.Engine(model)
        self.assertEqual(engine.output_scale, 1 / 256)  # Never /255.
        self.assertEqual(engine.output_zero, 0)
        for _ in range(400):
            self.assertFalse(engine.push(np.zeros(512, dtype=np.int16)))
        self.assertLessEqual(len(engine.frames), 2)
        self.assertLessEqual(len(engine.scores), 7)
        self.assertLessEqual(engine.pending.size, 160)

    def test_integrity_and_frozen_parameters(self):
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / 'bad.tflite'
            bad.write_bytes(model.read_bytes()[:-1] + b'X')
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                module.Engine(bad)
        with self.assertRaises(ValueError):
            module.Engine(model, 0.98)
        with self.assertRaises(ValueError):
            module.Engine(model, window=6)

    def test_wire_reset_eof_and_failure(self):
        data = b'A' + bytes(1024)
        result = subprocess.run([sys.executable, str(root / 'runtime/wake/amina_worker.py'), '--model', str(model)],
                                input=data + b'R' + data, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'RNRN')
        result = subprocess.run([sys.executable, str(root / 'runtime/wake/amina_worker.py'), '--model', str(model)],
                                input=b'A\0', capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'R')

    def test_requires_full_window(self):
        engine = module.Engine(model)
        class Interpreter:
            count = 0
            def set_tensor(self, *_): pass
            def invoke(self): self.count += 1
            def get_tensor(self, *_): return np.array([[254]], dtype=np.uint8)
        fake = Interpreter()
        engine.interpreter = fake
        while fake.count < 7:
            detected = engine.push(np.zeros(160, dtype=np.int16))
            self.assertEqual(detected, fake.count == 7)

    def test_output_dequantization_and_window(self):
        # Real model metadata with deterministic inference output: 253/256 < .99,
        # 254/256 > .99. This catches the unsafe /255 threshold change.
        engine = module.Engine(model)
        class Interpreter:
            def set_tensor(self, *_): pass
            def invoke(self): pass
            def get_tensor(self, *_): return np.array([[253]], dtype=np.uint8)
        engine.interpreter = Interpreter()
        for _ in range(30):
            self.assertFalse(engine.push(np.zeros(512, dtype=np.int16)))
        engine.interpreter.get_tensor = lambda *_: np.array([[254]], dtype=np.uint8)
        self.assertTrue(any(engine.push(np.zeros(512, dtype=np.int16)) for _ in range(30)))
        # Metadata, including nonzero zero_point, actually controls dequantization.
        engine.output_zero = 254
        engine.scores.clear()
        for _ in range(30):
            self.assertFalse(engine.push(np.zeros(512, dtype=np.int16)))


unittest.main()
