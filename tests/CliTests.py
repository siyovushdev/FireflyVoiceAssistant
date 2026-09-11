"""Production CLI with stub RKLLM; never touches the model/runtime/NPU."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    prompt = Path(directory) / "prompt.txt"
    prompt.write_text("frozen")
    args = [sys.argv[1], "--model", "fake", "--system-prompt", str(prompt)]
    cases = [
        (b"", b""),
        (b"\n \t\n", b"U\nU\n"),
        (b"quit\nignored\n", b""),
        (b" exit \nignored\n", b""),
        (("на кухне свет 1\n" * 3).encode(), b"L|K|1\n" * 3),
        ("Открой входную дверь\nОткрой входную дверь\n".encode(), b"D\nD\n"),
        (b"x" * 10000 + "\nна кухне свет 50\n".encode(), b"U\nL|K|50\n"),
        (b"\xff\n\x00\n", b"U\nU\n"),
        ("на кухне свет 99".encode(), b"L|K|99\n"),
    ]
    for text, expected in cases:
        result = subprocess.run(args, input=text, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout == expected, (result.stdout, expected)
        assert result.stderr.count(b"stub destroyed") == 1, result.stderr
    env = dict(os.environ, FIREFLY_TEST_INIT_FAIL="1")
    result = subprocess.run(args, input=b"", capture_output=True, env=env, timeout=10)
    assert result.returncode == 1 and result.stdout == b"U\n", result
print("10 CLI scenarios passed (EOF/Ctrl+D semantics, blanks, quit/exit, repeated commands, bounded input, startup failure)")
