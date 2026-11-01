#!/usr/bin/env python3
"""Verify and embed fixed full F2 utterances. No model/audio/network execution."""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import wave


def generate(root, output):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert manifest['profile'] == 'Supertonic3 INT8 / F2 SID1 / ru / CPU4 / steps5 / speed1 / sherpa-onnx1.13.7'
    clips = manifest['clips']
    assert 1 <= len(clips) <= 16
    seen = set()
    lines = ['// Generated from SHA256-verified full F2 sentences. Do not edit.',
             '#pragma once', '#include <cstdint>', '#include <string>', '#include <vector>',
             'namespace firefly::voice::f2 {']
    total = 0
    for index, row in enumerate(clips):
        name, text = row['file'], row['text']
        assert Path(name).name == name and name.endswith('.wav')
        assert text and len(text.encode()) <= 1024 and text not in seen
        seen.add(text)
        path = root / name
        assert not path.is_symlink()
        data = path.read_bytes()
        assert len(data) == row['size'] and hashlib.sha256(data).hexdigest() == row['sha256'], name
        with wave.open(io.BytesIO(data)) as w:
            assert (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getcomptype()) == (1, 2, 44100, 'NONE')
            assert 0 < w.getnframes() <= 20 * 44100
            pcm = w.readframes(w.getnframes())
            assert len(pcm) == w.getnframes() * 2
        total += len(pcm)
        assert total <= 8 * 1024 * 1024
        values = struct.unpack('<' + str(len(pcm) // 2) + 'h', pcm)
        assert max(abs(n) for n in values) <= 31458  # accepted pilot attenuates overshoot only
        lines.append(f'inline constexpr std::int16_t Stock{index}[] = {{')
        lines.extend(','.join(map(str, values[i:i+32])) + ',' for i in range(0, len(values), 32))
        lines.append('};')
    lines.append('inline std::vector<float> stockAudio(const std::string& text) {')
    for index, row in enumerate(clips):
        lines.append(f'    if (text == {json.dumps(row["text"], ensure_ascii=False)}) {{')
        lines.append(f'        std::vector<float> out; out.reserve(sizeof(Stock{index})/sizeof(*Stock{index}));')
        lines.append(f'        for (auto n : Stock{index}) out.push_back(n / 32767.f); return out; }}')
    lines += ['    return {};', '}', '}']
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
