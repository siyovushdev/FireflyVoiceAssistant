#!/usr/bin/env python3
"""Validate a prebuilt V2 allowlist and embed only declared assets in HttpServer.

Build-time only. No npm, network access, arbitrary runtime paths or directory walk
at request time. Manifest digests bind the generated code to one complete dist.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.woff2': 'font/woff2',
}


def read_assets(directory):
    root = Path(directory).resolve(strict=True)
    manifest_path = root / 'firefly-assets.json'
    if manifest_path.is_symlink() or not manifest_path.is_file() or manifest_path.stat().st_size > 65536:
        raise ValueError('V2 manifest must be a regular file, at most 64 KiB')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if set(manifest) != {'version', 'files'} or manifest['version'] != 1:
        raise ValueError('Unsupported V2 manifest format')
    if not isinstance(manifest['files'], list) or not 1 <= len(manifest['files']) <= 128:
        raise ValueError('V2 manifest requires 1..128 files')
    files = {}
    total = 0
    for entry in manifest['files']:
        if not isinstance(entry, dict) or set(entry) != {'file', 'sha256'}:
            raise ValueError('Invalid V2 asset entry')
        name, digest = entry['file'], entry['sha256']
        if not isinstance(name, str) or not re.fullmatch(r'index\.html|assets/[A-Za-z0-9_-]+\.(js|css|svg|png|woff2)', name):
            raise ValueError('Only index.html and flat hashed assets are allowed')
        if name in files or not isinstance(digest, str) or not re.fullmatch(r'[a-f0-9]{64}', digest):
            raise ValueError('Duplicate V2 file or invalid digest')
        path = root
        for part in name.split('/'):
            path = path / part
            if path.is_symlink():
                raise ValueError('V2 symlinks are forbidden')
        if not path.is_file() or root not in path.resolve(strict=True).parents:
            raise ValueError('V2 asset must be a contained regular file')
        if path.stat().st_size > 8 * 1024 * 1024:
            raise ValueError('V2 asset exceeds 8 MiB')
        body = path.read_bytes()
        total += len(body)
        if total > 16 * 1024 * 1024:
            raise ValueError('V2 bundle exceeds 16 MiB')
        if hashlib.sha256(body).hexdigest() != digest:
            raise ValueError('V2 asset digest mismatch: ' + name)
        files[name] = body
    if 'index.html' not in files:
        raise ValueError('V2 entry index.html missing')
    return files


def package(directory, output):
    files = read_assets(directory)
    lines = ['// Generated from verified dist/firefly-assets.json. Do not edit.',
             '#pragma once', '#include "firefly/admin/Api.h"', '#include <map>',
             'namespace firefly::admin {',
             'inline void addV2Assets(std::map<std::string, HttpReply>& assets) {']
    for name, body in sorted(files.items()):
        url = '/v2/' if name == 'index.html' else '/v2/' + name
        # Fixed-width octal string escapes support every byte, including NUL.
        encoded = [''.join('\\%03o' % byte for byte in body[start:start + 64])
                   for start in range(0, len(body), 64)] or ['']
        literal = '\n'.join('        "' + chunk + '"' for chunk in encoded)
        lines.append(f'    assets.emplace("{url}", HttpReply{{200, "{MIME[Path(name).suffix]}", std::string(\n{literal}, {len(body)})}});')
    lines += ['}', '}', '']
    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('\n'.join(lines), encoding='ascii')
    print(f'V2 allowlist: {len(files)} embedded assets, {sum(map(len, files.values()))} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dist')
    parser.add_argument('output')
    arguments = parser.parse_args()
    package(arguments.dist, arguments.output)
