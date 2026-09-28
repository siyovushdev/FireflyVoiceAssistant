import argparse
import sys
import time
parser = argparse.ArgumentParser()
parser.add_argument('--model')
parser.add_argument('--cutoff')
parser.add_argument('--window')
args = parser.parse_args()
if args.model == 'startup-failure':
    sys.exit(1)
sys.stdout.buffer.write(b'R')
sys.stdout.buffer.flush()
while True:
    cmd = sys.stdin.buffer.read(1)
    if not cmd:
        break
    if args.model == 'hang':
        time.sleep(30)
    if cmd == b'R':
        reply = b'R'
    else:
        data = sys.stdin.buffer.read(1024)
        if len(data) != 1024:
            sys.exit(1)
        if args.model == 'disconnect':
            sys.exit(1)
        reply = b'X' if args.model == 'malformed' else b'W'
    sys.stdout.buffer.write(reply)
    sys.stdout.buffer.flush()
