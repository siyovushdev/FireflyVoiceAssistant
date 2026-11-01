#!/usr/bin/env python3
"""Product PCM-only KWS worker. Never opens audio or calls the command runtime.

Wire v1: ready R; request A + 512 little-endian int16 samples -> N/W;
request R -> reconstruct streaming state -> R. EOF exits. Errors exit nonzero.
Frozen frontend/quantization/window semantics follow the accepted shadow runner.
"""
import argparse
from collections import deque
import hashlib
import sys
from pathlib import Path

import numpy as np
from ai_edge_litert.interpreter import Interpreter
from pymicro_features import MicroFrontend

MODEL_SHA256 = "24000c0e3f9fd3249f56e5f836719aba8f7a3f23c1f4a2c571bdf0bf6cc8a1dd"


class Engine:
    def __init__(self, model, cutoff=0.99, window=7):
        if cutoff != 0.99 or window != 7:
            raise ValueError("Amina V1 cutoff/window are frozen")
        # Load the exact verified bytes, avoiding a path reopen after verification.
        content = Path(model).read_bytes()
        if hashlib.sha256(content).hexdigest() != MODEL_SHA256:
            raise ValueError("Amina model SHA256 mismatch")
        self.interpreter = Interpreter(model_content=content)
        self.interpreter.allocate_tensors()
        inputs = self.interpreter.get_input_details()
        outputs = self.interpreter.get_output_details()
        if len(inputs) != 1 or len(outputs) != 1:
            raise ValueError("Unexpected model IO count")
        self.input, self.output = inputs[0], outputs[0]
        if self.input['dtype'] != np.int8 or list(self.input['shape']) != [1, 3, 40]:
            raise ValueError("Unexpected Amina input")
        if self.output['dtype'] != np.uint8 or list(self.output['shape']) != [1, 1]:
            raise ValueError("Unexpected Amina output")
        self.input_scale, self.input_zero = self.quantization(self.input)
        self.output_scale, self.output_zero = self.quantization(self.output)
        details = self.interpreter.get_tensor_details()
        states = [d for d in details if any(t in d['name'].lower() for t in ('state', 'variable', 'assign'))]
        handles = [d for d in details if d['dtype'] == np.object_ and 'states' in d['name']]
        if len(states) != 14 or len(handles) != 6:
            raise ValueError("Unexpected Amina streaming states")
        # Never read resource handles. LiteRT owns all 14 state tensors across invokes.
        self.frontend = MicroFrontend()
        self.pending = np.zeros(0, dtype=np.int16)
        self.frames = []
        self.scores = deque(maxlen=window)
        self.cutoff, self.window = cutoff, window

    @staticmethod
    def quantization(detail):
        params = detail['quantization_parameters']
        if len(params['scales']) != 1 or len(params['zero_points']) != 1:
            raise ValueError("Expected per-tensor quantization")
        scale, zero = float(params['scales'][0]), int(params['zero_points'][0])
        if not np.isfinite(scale) or scale <= 0:
            raise ValueError("Invalid quantization scale")
        return scale, zero

    def push(self, mono):
        if mono.dtype != np.int16 or mono.ndim != 1:
            raise ValueError("Expected mono int16")
        self.pending = np.concatenate((self.pending, mono))
        wake = False
        while self.pending.size > 160:
            result = self.frontend.process_samples(self.pending[:160].tobytes())
            read = int(result.samples_read)
            if not 0 < read <= 160:
                raise ValueError("Invalid frontend consumption")
            self.pending = self.pending[read:]
            if not result.features:
                continue
            self.frames.append(np.asarray(result.features, dtype=np.float32))
            if len(self.frames) != 3:
                continue
            spec = np.stack(self.frames, axis=0)
            self.frames.clear()
            # Preserve accepted reference conversion (float32 then int8 cast).
            quantized = (spec / np.float32(self.input_scale) + np.float32(self.input_zero)).astype(np.int8)
            self.interpreter.set_tensor(self.input['index'], quantized.reshape(1, 3, 40))
            self.interpreter.invoke()
            raw = self.interpreter.get_tensor(self.output['index'])[0][0]
            score = self.output_scale * (float(raw) - self.output_zero)
            self.scores.append(score)
            if len(self.scores) == self.window and sum(self.scores) / self.window >= self.cutoff:
                wake = True
        # Admission, single session, and reset are owned by the C++ state machine.
        return wake


def read_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        block = stream.read(size - len(data))
        if not block:
            raise EOFError("Truncated PCM request")
        data.extend(block)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', required=True)
    parser.add_argument('--cutoff', type=float, default=0.99)
    parser.add_argument('--window', type=int, default=7)
    args = parser.parse_args()
    engine = Engine(args.model, args.cutoff, args.window)
    incoming, outgoing = sys.stdin.buffer, sys.stdout.buffer
    outgoing.write(b'R'); outgoing.flush()
    while True:
        command = incoming.read(1)
        if not command:
            return
        if command == b'R':
            engine = Engine(args.model, args.cutoff, args.window)
            reply = b'R'
        elif command == b'A':
            mono = np.frombuffer(read_exact(incoming, 1024), dtype='<i2')
            reply = b'W' if engine.push(mono) else b'N'
        else:
            raise ValueError("Unknown KWS request")
        outgoing.write(reply); outgoing.flush()


if __name__ == '__main__':
    main()
