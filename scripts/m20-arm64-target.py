#!/usr/bin/env python3
"""Run target userspace in a PRIVATE user namespace; never register host binfmt."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host-user-ns', required=True)
    parser.add_argument('--unprivileged-test-user', action='store_true',
                        help='Run isolated CPU tests as mapped uid/gid1000, never staging')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if os.getuid() != 0 or os.readlink('/proc/self/ns/user') == args.host_user_ns:
        parser.error('Requires a new unshare -Ur -m namespace; host registration is forbidden')
    sdk, output = args.sdk.resolve(), args.output.resolve()
    subprocess.run(['mount', '--make-rprivate', '/'], check=True)
    with tempfile.TemporaryDirectory(prefix='binfmt-', dir=output / 'tmp') as temporary:
        root = Path(temporary)
        subprocess.run(['mount', '-t', 'binfmt_misc', 'binfmt_misc', str(root)], check=True)
        if {p.name for p in root.iterdir()} != {'register', 'status'}:
            raise RuntimeError('Refusing a binfmt instance with inherited/global entries')
        magic = bytes([127, 69, 76, 70, 2, 1, 1, 0] + [0] * 8 + [2, 0, 183, 0])
        mask = bytes([255] * 7 + [0] + [255] * 8 + [254, 255, 255, 255])
        encode = lambda data: ''.join(chr(92) + 'x%02x' % b for b in data)
        record = ':firefly-arm64:M::' + encode(magic) + ':' + encode(mask) + ':' + str(sdk / 'host-tools/usr/bin/qemu-aarch64-static') + ':F'
        (root / 'register').write_text(record)
        (output / 'private-binfmt-proof.json').write_text(json.dumps({
            'user_namespace': os.readlink('/proc/self/ns/user'), 'host_user_namespace': args.host_user_ns,
            'instance': str(root), 'handler': (root / 'firefly-arm64').read_text(),
            'global_registration': False}, indent=2))
        box = ['bwrap', '--unshare-net', '--die-with-parent', '--tmpfs', '/']
        if args.unprivileged_test_user:
            box += ['--unshare-user', '--uid', '1000', '--gid', '1000']
        # A tmpfs namespace root supplies mountpoints without changing the SDK.
        # Every target-root entry stays readonly; only explicit work mounts write.
        for child in sorted((sdk / 'rootfs').iterdir()):
            if child.name in {'dev', 'proc', 'tmp', 'opt'}:
                continue
            box += (['--symlink', str(child.readlink()), '/' + child.name] if child.is_symlink()
                    else ['--ro-bind', str(child), '/' + child.name])
        box += ['--dev', '/dev', '--proc', '/proc', '--tmpfs', '/tmp',
               '--setenv', 'PATH', '/usr/bin:/bin', '--setenv', 'PYTHONDONTWRITEBYTECODE', '1',
               '--setenv', 'GIT_CONFIG_NOSYSTEM', '1', '--setenv', 'GIT_CONFIG_GLOBAL', '/dev/null',
               '--setenv', 'GIT_CONFIG_COUNT', '0', '--setenv', 'GIT_OPTIONAL_LOCKS', '0',
               '--setenv', 'PYTHONNOUSERSITE', '1', '--unsetenv', 'LD_LIBRARY_PATH', '--unsetenv', 'LD_PRELOAD']
        for source, target, writable in [
                (output / 'source', '/src/firefly', False), (output / 'build', '/build/firefly', True),
                (sdk / 'frozen-m19', '/deps/frozen', False), (sdk / 'include', '/deps/include', False),
                (sdk / 'mosquitto-install', '/deps/mqtt', False), (sdk / 'rootfs', '/target', False),
                (sdk / 'mqtt-provenance.json', '/deps/mqtt-provenance.json', False), (output, '/work', True)]:
            box += ['--bind' if writable else '--ro-bind', str(source), target]
        box += ['--ro-bind', str(sdk / 'frozen-m19/share/firefly/wake/amina-v3.tflite'),
                '/opt/firefly/voice-assistant/releases/grounding-818157ec-20261013T040322Z/share/firefly/wake/amina-v3.tflite']
        command = args.command[1:] if args.command[:1] == ['--'] else args.command
        if not command:
            parser.error('Target command is required')
        try:
            p = subprocess.run(box + ['--', *command], check=False)
        finally:
            # F holds an open interpreter inode. Explicitly unregister before
            # unmount so no pinned handler survives this private fixture.
            (root / 'firefly-arm64').write_text('-1')
            subprocess.run(['umount', str(root)], check=True)
        return p.returncode


if __name__ == '__main__':
    raise SystemExit(main())
