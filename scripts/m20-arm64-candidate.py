#!/usr/bin/env python3
"""Offline orchestration of the existing CMake and stage-release pipeline.

The SDK is separately provisioned and explicitly hash-approved. No installer,
downloader, global binfmt, systemd operation or activation is used here.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def verify_sdk(sdk, pin):
    manifest = sdk / 'sdk-manifest.json'
    if digest(manifest) != pin:
        raise ValueError('SDK manifest does not match the explicitly approved SHA256')
    data = json.loads(manifest.read_text())
    if data['format'] != 1 or data['target'] != 'linux/aarch64 Ubuntu22.04':
        raise ValueError('Wrong SDK target')
    for name, item in data['files'].items():
        rel = Path(name)
        if rel.is_absolute() or '..' in rel.parts:
            raise ValueError('Unsafe SDK input path')
        p = sdk / rel
        if item['kind'] == 'symlink':
            good = p.is_symlink() and str(p.readlink()) == item['target']
        elif item['kind'] == 'directory':
            good = not p.is_symlink() and p.is_dir()
        else:
            good = not p.is_symlink() and p.is_file() and p.stat().st_size == item['bytes'] and digest(p) == item['sha256']
        if not good or p.lstat().st_mode & 0o7777 != item['mode']:
            raise ValueError('SDK input changed: ' + name)
    if 'roots' in data:
        present = set()
        for name in data['roots']:
            root = sdk / name
            present.add(name)
            if root.is_dir() and not root.is_symlink():
                present.update(str(p.relative_to(sdk)) for p in root.rglob('*'))
        if present != set(data['files']):
            raise ValueError('SDK has undeclared/missing inputs')


def sandbox(sdk, output):
    args = ['bwrap', '--unshare-user', '--unshare-net', '--die-with-parent', '--tmpfs', '/',
            '--ro-bind', '/usr', '/usr', '--symlink', 'usr/bin', '/bin',
            '--symlink', 'usr/lib', '/lib', '--symlink', 'usr/lib64', '/lib64',
            '--ro-bind', '/etc', '/etc', '--dev', '/dev', '--proc', '/proc', '--tmpfs', '/tmp',
            '--setenv', 'PATH', '/usr/bin:/bin', '--setenv', 'LD_LIBRARY_PATH', '/tool-libs',
            '--setenv', 'GIT_CONFIG_NOSYSTEM', '1', '--setenv', 'GIT_CONFIG_GLOBAL', '/dev/null',
            '--setenv', 'GIT_CONFIG_COUNT', '0', '--setenv', 'GIT_OPTIONAL_LOCKS', '0',
            '--setenv', 'PYTHONDONTWRITEBYTECODE', '1']
    for source, target, writable in [
            (sdk / 'toolchain', '/sdk', False), (sdk / 'rootfs', '/target', False),
            (sdk / 'host-tools', '/tools', False), (sdk / 'tool-libs', '/tool-libs', False),
            (sdk / 'frozen-m19', '/deps/frozen', False), (sdk / 'include', '/deps/include', False),
            (sdk / 'mosquitto-install', '/deps/mqtt', False),
            (sdk / 'mqtt-provenance.json', '/deps/mqtt-provenance.json', False),
            (sdk / 'cross.cmake', '/cross.cmake', False),
            (sdk / 'frozen-m19/share/firefly/wake/amina-v3.tflite',
             '/opt/firefly/voice-assistant/releases/grounding-818157ec-20261013T040322Z/share/firefly/wake/amina-v3.tflite', False),
            (output / 'source', '/src/firefly', False), (output / 'build', '/build/firefly', True),
            (output, '/work', True)]:
        args += ['--bind' if writable else '--ro-bind', str(source), target]
    return args


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--sdk', type=Path, required=True)
    ap.add_argument('--sdk-manifest-sha256', required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--release-id', required=True)
    args = ap.parse_args()
    repo, sdk, output = (p.resolve() for p in (args.repo, args.sdk, args.output))
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,80}', args.release_id):
        ap.error('Invalid release id')
    if output.exists():
        ap.error('Output must be NEW')
    if subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], text=True):
        ap.error('Source must be clean and committed')
    verify_sdk(sdk, args.sdk_manifest_sha256)
    if not (repo / 'src/admin/frontend/node_modules').is_dir():
        ap.error('Pre-provision matching frontend dependencies; no installs are performed')
    os.umask(0o002)
    output.mkdir(parents=True)
    for name in ('build', 'logs', 'tmp'):
        (output / name).mkdir()
    records = []
    env = dict(os.environ)
    for name in ('DISPLAY', 'FIREFLY_OWNER_ACCEPTED', 'LD_LIBRARY_PATH', 'LD_PRELOAD'):
        env.pop(name, None)
    head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    env['SOURCE_DATE_EPOCH'] = subprocess.check_output(['git', '-C', str(repo), 'show', '-s', '--format=%ct', head], text=True).strip()

    def run(name, argv):
        with (output / 'logs' / (name + '.log')).open('w') as log:
            p = subprocess.run(list(map(str, argv)), cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT)
        records.append({'name': name, 'argv': list(map(str, argv)), 'exit': p.returncode})
        (output / 'commands.json').write_text(json.dumps(records, indent=2))
        print(name, p.returncode, flush=True)
        p.check_returncode()

    run('clone', ['git', 'clone', '--quiet', '--no-hardlinks', '--no-checkout', repo, output / 'source'])
    run('checkout', ['git', '-C', output / 'source', 'checkout', '--quiet', '--detach', head])
    shutil.copytree(repo / 'src/admin/frontend/node_modules', output / 'source/src/admin/frontend/node_modules', symlinks=True)
    frontend = output / 'source/src/admin/frontend'
    for script in ['typecheck', 'test', 'build']:
        run('frontend-' + script, ['npm', '--prefix', frontend, *(['test'] if script == 'test' else ['run', script])])
    box = sandbox(sdk, output)
    # Frontend output is generated before the source mount becomes readonly.
    flags = ['-DCMAKE_TOOLCHAIN_FILE=/cross.cmake', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=ON',
             '-DFIREFLY_HOST_OFFLINE_TESTS=OFF', '-DFIREFLY_BUILD_ASSISTANT=ON', '-DFIREFLY_BUILD_VOICE=ON',
             '-DFIREFLY_BUILD_ADMIN=ON', '-DFIREFLY_BUILD_CONFIG_TOOLS=ON', '-DFIREFLY_ENABLE_MQTT=ON',
             '-DFIREFLY_MQTT_NAMESPACE_TESTS=ON', '-DFIREFLY_TEST_MOSQUITTO_SERVER=/deps/mqtt/sbin/mosquitto',
             '-DFIREFLY_BUILD_LOCAL_MQTT_CONTROLLER=ON', '-DRKLLM_INCLUDE_DIR=/deps/include',
             '-DRKLLM_LIBRARY=/deps/frozen/lib/librkllmrt.so', '-DSHERPA_ONNX_ROOT=/deps/frozen',
             '-DSHERPA_ONNX_INCLUDE_DIR=/deps/include', '-DSHERPA_ONNX_LIBRARY=/deps/frozen/lib/libsherpa-onnx-c-api.so',
             '-DFIREFLY_ADMIN_DIST_DIR=/src/firefly/src/admin/frontend/dist',
             '-DFIREFLY_MOSQUITTO_INCLUDE_DIR=/deps/mqtt/include', '-DFIREFLY_MOSQUITTO_LIBRARY=/deps/mqtt/lib/libmosquitto.so',
             '-DFIREFLY_MQTT_PACKAGE_ROOT=/deps/mqtt', '-DFIREFLY_TLS_PACKAGE_ROOT=/target',
             '-DFIREFLY_WAKE_PYTHON=/deps/frozen/share/firefly/wake/python/bin/python',
             '-DFIREFLY_WAKE_MODEL_REFERENCE=/opt/firefly/voice-assistant/releases/grounding-818157ec-20261013T040322Z/share/firefly/wake/amina-v3.tflite',
             '-DFIREFLY_MQTT_PROVENANCE=/deps/mqtt-provenance.json']
    run('configure-arm64', box + ['--', 'cmake', '-S', '/src/firefly', '-B', '/build/firefly', *flags])
    run('build-arm64', box + ['--', 'cmake', '--build', '/build/firefly', '--parallel', '2'])
    run('preinstall-arm64', box + ['--', 'cmake', '--build', '/build/firefly', '--target', 'preinstall', '--parallel', '2'])
    stage = ['python3', '/src/firefly/scripts/stage-release.py', 'stage', '--offline-candidate',
             '--source-root', '/src/firefly', '--build-dir', '/build/firefly', '--destination', '/work/candidate',
             '--release-id', args.release_id, '--runtime-prefix', '/opt/firefly/voice-assistant',
             '--rkllm-library', '/deps/frozen/lib/librkllmrt.so', '--sherpa-library', '/deps/frozen/lib/libsherpa-onnx-c-api.so',
             '--onnx-library', '/deps/frozen/lib/libonnxruntime.so',
             '--wake-site-packages', '/deps/frozen/share/firefly/wake/python/lib/python3.10/site-packages',
             '--mqtt-root', '/deps/mqtt', '--tls-root', '/target', '--mqtt-provenance', '/deps/mqtt-provenance.json']
    target = ['unshare', '-Ur', '-m', '--', sys.executable, str(Path(__file__).with_name('m20-arm64-target.py')),
              '--sdk', str(sdk), '--output', str(output), '--host-user-ns', os.readlink('/proc/self/ns/user'), '--']
    run('stage-candidate', target + stage)
    run('verify-candidate', target + ['python3', '/src/firefly/scripts/stage-release.py',
        'verify-candidate', '--destination', '/work/candidate', '--release-id', args.release_id])
    verify_sdk(sdk, args.sdk_manifest_sha256)
    if subprocess.check_output(['git', '-C', str(output / 'source'), 'status', '--porcelain'], text=True):
        raise ValueError('Build/staging mutated tracked source')
    print('ARM64_CANDIDATE_STAGED', head, 'OWNER_ACTIVATION_REQUIRED', flush=True)


if __name__ == '__main__':
    main()
