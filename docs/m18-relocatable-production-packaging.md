# M18.6 — Relocatable production packaging

Baseline: `e5acc711dbe29aacd68a56b6e96f8554f1618d58`.
Branch: `feature/m18.6-relocatable-deployment`.
This milestone packages the existing single-process appliance. It does not change
speech models, wake thresholds, SafetyGate, RKLLM inference, device binding, or
the Web Admin UI. It does not install into `/opt` or `/etc`, and it does not
restart the running production service.

## Audited hardcoded paths

The M18.1 unit and `scripts/firefly-dialogue.sh` started `build/firefly-demo`
from the Git checkout. The audited runtime dependencies were:

| Dependency | Previous location |
| --- | --- |
| `firefly-demo` | `/home/firefly/FireflyVoiceAssistantLinux/build/firefly-demo` |
| ELF RUNPATH | `/home/firefly/demo_Linux_aarch64/lib` and the sherpa build `lib` |
| Process library path | `LD_LIBRARY_PATH=/home/firefly/demo_Linux_aarch64/lib` |
| Runtime RKLLM model | `/home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm` |
| Admin 4096 model | hidden string in `RuntimeModel.h` |
| Empty seed | checkout `config/empty-home.json` |
| STT / VAD | checkout `build/stt/model` and `build/stt/vad-ab/silero_vad.onnx` |
| Wake interpreter | checkout `build/wake-venv/bin/python` |
| Amina model | checkout `assets/wake/amina-v3.tflite` |
| Working directory | the Git checkout |

`readelf -d` on the previous `firefly-demo` showed absolute RUNPATH entries for
those two directories. `ldd` resolved `librkllmrt.so` and `libsherpa-onnx-c-api.so`
only because of that RUNPATH plus `LD_LIBRARY_PATH`. Transitive dependencies:

- `librkllmrt.so` needs only system libraries (`libgomp`, `libpthread`, `libstdc++`, `libdl`, `libm`, `libc`).
- `libsherpa-onnx-c-api.so` needs `libonnxruntime.so` through its own `$ORIGIN` RPATH, plus system libraries.
- `libonnxruntime.so` needs system libraries and has `$ORIGIN` RPATH.
- `libasound.so.2` stays the Ubuntu 22.04 system library.

The wake virtualenv's `python3` is a symlink to `/usr/bin/python3` (3.10.12).
`pyvenv.cfg` says `home = /usr/bin`. `pip` scripts in that venv have a shebang
pointing at the venv path. Copying the venv would keep those shebangs. Native
extensions do not: NumPy OpenBLAS/gfortran use `$ORIGIN/../../numpy.libs`, and
LiteRT uses `$ORIGIN`. No `/home/firefly` string remains in the `.py` sources
after `__pycache__` is excluded. Importing from a copied site-packages tree
does not write into `$HOME` or the package directory.

Other absolute paths that are **not** part of the production process:

- `benchmarks/language-index/ProductionAcceptance.cpp`, `benchmarks/web-admin-b3/Acceptance.cpp`, and `benchmarks/dialogue-setup/Acceptance.cpp` pass both model paths explicitly. They are owner NPU harnesses and are not installed.
- `scripts/v1-demo-run.sh`, `scripts/v1-wake-live.sh`, and historical milestone docs still name the SDK. They are not the production unit.
- `firefly-admin` / `firefly-admin-runtime` still receive only `--model`. A setup attempt in that separate editor fails closed with `admin_model_not_configured`. Production setup runs inside `firefly-demo`.
- If `--stt-model-dir` is omitted, `firefly-demo` may still look next to the executable under `stt/model`. The production unit always passes the shared STT directory.

No local wheelhouse for LiteRT, pymicro-features, or NumPy was found. The
offline source is the already installed wake `site-packages` directory.

## Production layout

```text
/opt/firefly/voice-assistant/
    current -> releases/<release-id>/          # relative symlink, owner-activated
    previous-release                           # one release id, written on activate
    releases/<release-id>/
        bin/firefly-demo                       # stripped, RUNPATH $ORIGIN/../lib
        bin/firefly-config                     # target release's read-only auditor
        lib/librkllmrt.so
        lib/libsherpa-onnx-c-api.so
        lib/libonnxruntime.so
        share/firefly/config/empty-home.json
        share/firefly/wake/amina-v3.tflite
        share/firefly/wake/amina_worker.py
        share/firefly/wake/manifest.json
        share/firefly/wake/requirements.txt
        share/firefly/wake/python/bin/python   # relocatable wrapper
        share/firefly/wake/python/lib/python3.10/site-packages/
        share/firefly/systemd/firefly-voice-assistant.service
        share/firefly/doc/
        share/firefly/deployment/scripts/stage-release.py
        share/firefly/deployment/packaging/__init__.py
        share/firefly/deployment/packaging/firefly_release.py
        manifest.json
        manifest.sha256
    models/                                    # shared across releases
        runtime/Qwen3-4B_W8A8_RK3588_3core.rkllm
        admin/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm
        stt/encoder.int8.onnx
        stt/decoder.onnx
        stt/joiner.int8.onnx
        stt/tokens.txt
        vad/silero_vad.onnx

/var/lib/firefly/store/                        # unchanged persistent state
    configuration.json
    backups/
    last_known_good.json
    last_known_good_snapshot.json

/etc/systemd/system/firefly-voice-assistant.service   # owner installs this later
```

`current` is not created by staging. Large models live beside `releases/` so an
application update does not copy them again. Web Admin assets stay inside
`firefly-demo` through the existing verified embedding; the package does not
ship `node_modules` or a frontend source tree.

## Library strategy

`firefly-demo` keeps an absolute build-tree RUNPATH so `./build/firefly-demo`
can still find the SDK during development. `cmake --install --component
firefly-runtime` rewrites the installed binary to exactly `$ORIGIN/../lib`.
The staging tool then strips that copy and places the three shared libraries
beside it. `libsherpa-onnx-c-api.so` continues to find `libonnxruntime.so`
through `$ORIGIN`. System libraries (`libc`, `libstdc++`, `libasound`,
`libgomp`) are not bundled.

The packaged unit sets no `LD_LIBRARY_PATH`. A staged binary is checked with
`readelf -d` and with `ldd` in an environment that has no `LD_LIBRARY_PATH`.
`-ffile-prefix-map` rewrites `__FILE__` so the installed binary does not embed
the checkout path in exception text.

## Models and integrity

Production mode (`--production`) refuses to publish unless the on-device bytes
match the frozen identities:

| Artifact | SHA256 |
| --- | --- |
| Runtime Qwen3-4B W8A8 | `d806976a3d77f2252a6abe40a5fd7d9b1006a90f671992923ac2c1542456ba8b` |
| Admin CTX4096 | `58e8e74d8f129ea6f68b8e2101aae65d512668f46bb847a89e88a6a6d6094e5b` (4,848,176,220 bytes) |
| `librkllmrt.so` | `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6` |
| `libsherpa-onnx-c-api.so` | `5997c35aa68c8843b2ddfa6208b6961a07f171fa636633dd8941270e1aaacf5f` |
| `libonnxruntime.so` | `d860f5968f5a1ed63533e9ed198aa747ca9fe289028129877f428556089f6874` |
| STT encoder / decoder / joiner / tokens | hashes in `docs/v1-artifact-manifest.md` |
| Silero VAD | `9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6` |
| `amina-v3.tflite` | `bc42cb869d2aace69c5bdf9333c502b96dedfa85df97676be85f5c289dc057d1` (60,912 bytes) |

Amina is checked in every mode because it is a source-tree artifact. Fixture
mode, used by automated tests, records the actual hashes of stand-in models
and still rejects empty or missing files. An existing shared model with a
different hash is left untouched and the stage fails. Copies are regular
files, not symlinks and not hardlinks to the SDK, so deleting the SDK path
cannot change the installed bytes.

`firefly-demo --admin-model` is optional. When it is set, startup stats the
file. A missing path, a symlink, or the wrong size is logged as
`STARTUP admin model unavailable` and does not load a model. The listener
still comes up, matching the existing rule that Admin stays available when
speech or model files are absent. `enterSetup` repeats the size check and
requires the SHA256 before any Admin inference. An empty path fails that
entry with `admin_model_not_configured`. A wrong size or hash fails with
`admin_model_unavailable_or_wrong_size` or `admin_model_sha256_mismatch`.
There is no download and no fallback path.
The checks open with `O_NOFOLLOW | O_NONBLOCK`, so a symlink or FIFO cannot
redirect or indefinitely block startup validation.

The empty seed remains `config/empty-home.json`: schema 2, no rooms, devices,
actions, bindings, connections, or observations.

## Python wake interpreter

The release does not copy `wake-venv`. CMake installs `packaging/wake-python`
as `share/firefly/wake/python/bin/python`. That wrapper puts the release
`site-packages` on `PYTHONPATH`, sets `PYTHONNOUSERSITE` and
`PYTHONDONTWRITEBYTECODE`, and execs `/usr/bin/python3`. Staging copies only
the runtime packages from the local site-packages tree (NumPy 2.2.6,
ai-edge-litert 1.2.0, pymicro-features 2.0.2, their dist-info, flatbuffers,
and NumPy's bundled OpenBLAS/gfortran). `pip`, setuptools, and `__pycache__`
are omitted.
The wrapper unsets `PYTHONHOME` and replaces, rather than appends to,
`PYTHONPATH`; developer Python environment values cannot override the release.

Verification imports those packages from the staged tree and starts
`amina_worker.py` until it writes the ready byte `R`. The worker is still
resolved by `firefly-demo` from `share/firefly/wake/amina_worker.py`, next to
the frozen model. The production unit passes that interpreter and model
explicitly. Cutoff `0.99` and window `7` are unchanged.

`/usr/bin/python3` 3.10 is an Ubuntu 22.04 interpreter, not a copy of the
developer venv. Staging fails if it is not Python 3.10 or if the pinned
imports fail. There is no network install.
The ready-byte probe has a fixed deadline and always waits for its child
after termination or killing. It must not leave an orphan after failure.

## Staging and the manifest

Fixture staging, used by tests and by a review tree, omits `--production`.
It records `source.identity` as `dirty-worktree` when `git status --porcelain`
is non-empty. That identity means HEAD does not describe the binary. A clean
worktree staged without `--production` is `committed-fixture`.

`--production` is refused while the worktree is dirty or untracked. It is the
only path that sets `source.identity` to `committed-production` and
`production_identity_checked` to true. This worktree is dirty, so that path
cannot be run until a later, separately approved commit. Do not treat the
current HEAD as the identity of the M18.6 binary.
Both shipped executables expose the same configure-time build identity:
HEAD, dirty flag, source fingerprint, embedded Admin asset fingerprint, and
build time. Staging compares those identities with the current inputs and
rechecks them before production publication. A binary configured while dirty
cannot later be relabelled as a clean committed release. Reconfigure and
rebuild after the owner separately approves a commit. Source fingerprints
include root CMake, C++/headers, third-party headers, configuration, and the
Admin packaging script; frontend assets are fingerprinted separately.

```sh
python3 scripts/stage-release.py stage \
  --destination /tmp/firefly-m186-review \
  --build-dir build \
  --source-root . \
  --runtime-prefix /opt/firefly/voice-assistant \
  --rkllm-library /home/firefly/demo_Linux_aarch64/lib/librkllmrt.so \
  --sherpa-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libsherpa-onnx-c-api.so \
  --onnx-library /home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu/lib/libonnxruntime.so \
  --runtime-model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_CTX4096_RK3588_3core.rkllm \
  --stt-dir /home/firefly/FireflyVoiceAssistantLinux/build/stt/model \
  --vad-model /home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab/silero_vad.onnx \
  --wake-site-packages /home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/lib/python3.10/site-packages \
  --production
```

`--runtime-prefix /opt/firefly/voice-assistant` makes the unit inside the
review tree name the final paths. The files themselves are written only under
`--destination`. Staging refuses `/opt`, `/etc`, `/usr`, `/var`, the production
checkout, and the RKLLM SDK directory. It also refuses a destination inside
this source tree.

The tool checks free space, including a 64 MiB margin, before copying. Models
already present with the same SHA256 are reused and are not copied into the
work tree. New models are published one file at a time. If a later file fails,
including `ENOSPC` or a file that appeared in the meantime, only the files
this attempt created are removed. An existing different checksum is left in
place. The release directory appears only after `rename` into `releases/<id>`.
An existing release id is not overwritten. `current` and `previous-release`
are not changed. A failed copy removes `.staging-*` and `.incoming-*` only.

`manifest.json` records the release id, source commit, dirty flag, identity,
architecture, pinned dependency versions, artifact checksums, shared-model
checksums, and `config_schema.readable` of `[1, 2]`, the versions
`ConfigLoader` accepts. `manifest.sha256` covers the manifest bytes.
`systemd.installed` is false.

Distribution class:

- `librkllmrt.so` and both `.rkllm` models are **same-device only**. The local SDK has no license file that grants redistribution.
- sherpa-onnx 1.13.7 is Apache-2.0 and ONNX Runtime is MIT, but this bundle ships no upstream license text. Do not copy those libraries off the device without the upstream notices.
- The application binary, empty seed, Amina worker, and unit template are project artifacts.

The manifest does not contain ConfigStore documents, credentials, or developer
home paths. Shared models are copies. They are not hardlinks to the SDK, so a
later `chown` of the install tree does not change the SDK inodes.

First production copy size, from read-only `stat` on 2026-10-10:

| Bytes | Artifact |
| --- | --- |
| 4,840,064,228 | runtime RKLLM model |
| 4,848,176,220 | Admin RKLLM model |
| 71,190,073 | STT files plus Silero VAD |
| 46,179,856 | `librkllmrt.so`, sherpa, ONNX Runtime |

The two models alone are 9,688,240,448 bytes. With speech models, libraries,
the wake tree, and the 64 MiB margin, the first stage needs about 9.3 GiB
free. The takeover measurement was 7,143,620,608 bytes free (about 6.65 GiB);
final post-test measurement was 7,123,521,536 bytes (about 6.63 GiB).
Recheck before acceptance. Repeat releases skip matching models, which does
not solve the first copy. Production installation stays blocked until space
exists without deleting the SDK or the live checkout.
Stage-then-install on one filesystem needs room for two complete bundles,
about 18.6 GiB plus operating headroom. Prefer at least 25 GiB free on an
owner-approved Linux filesystem supporting permissions, symlinks, hardlinks,
and atomic no-replace renames. A separate empty `models/` mount is insufficient:
installation copies new models into the prefix before same-filesystem rename.
Stage and install the whole prefix on external storage, with an approved
persistent mount at the fixed production prefix, or separately approve a
verified model prepopulation/reuse procedure. Neither option is implemented
or executed as a storage change by this review.

## Activation, install, and rollback

Staging does not activate. Installing a release into another prefix does not
activate it either:

```sh
python3 scripts/stage-release.py verify --destination PREFIX --release-id ID
python3 scripts/stage-release.py install-release \
  --source REVIEW_PREFIX --destination DEST_PREFIX --release-id ID
python3 scripts/stage-release.py install-unit \
  --source UNIT --destination UNIT_DEST --backup-dir BACKUP_DIR
python3 scripts/stage-release.py activate \
  --destination DEST_PREFIX --release-id ID --store STORE
python3 scripts/stage-release.py rollback --destination DEST_PREFIX --store STORE
python3 scripts/stage-release.py restore-unit --backup SAVED_UNIT --destination UNIT_DEST
```

`--store` is required for `activate` and `rollback`. A missing file, a
symlink, unreadable bytes, malformed JSON, a non-integer `schema_version`, or
a version outside `[1, 2]` leaves `current` unchanged. The audit also applies
the real C++ loader's complete document, canonical-byte, digest, and private
store contract without constructing a writable ConfigStore or creating its
lock. Configuration and directory owners must agree, including under root.
The shipped target release's `bin/firefly-config` is the default auditor;
production forbids a substituted auditor and requires exactly
`/var/lib/firefly/store`, owned by `firefly`. `--fresh` is limited to fixture
activation with `configuration.json` absent and is refused in production. None of
these commands write the store or run `systemctl`.

`/opt/firefly/voice-assistant` and the live unit path are refused unless
`FIREFLY_OWNER_ACCEPTED=1`. The tool still does not restart the service.
`install-release` preserves existing releases, matching models, `current`, and
`previous-release`. `install-unit` copies the existing unit into the backup
directory before `rename` replaces the destination. `restore-unit` puts that
backup back with the same `rename` and leaves the prefix on disk. Symlink
updates create a temporary symlink and `rename` it over `current`. `ln -sfn`
is not the rollback primitive. If the previous-release marker cannot be
written after the symlink moves, the tool switches `current` back.
Mutation is serialized by a nonblocking prefix lock. Prefixes must be owned
by the invoking user and not writable by group or others; production
artifacts must be root-owned and not writable by the service user. Prefix
components, release artifacts, and model trees reject symlinks, hardlinks,
special files, traversal, duplicate manifest paths, and unexpected entries.
Only the exact production prefix, live unit, and designated unit-backup path
can receive privileged writes. Release publication uses
`renameat2(RENAME_NOREPLACE)`, including when an empty target appears during
installation. Scratch directories and partial copies are uniquely owned by
the attempt. Only newly published models are removed on failure; existing
models, releases, markers, and rollback archives are preserved. Unit backups
are published exclusively before the unit is replaced.

Rollback re-verifies the target release, including the wake worker, before
changing `current`. The live store is not read unless
`FIREFLY_OWNER_ACCEPTED=1`.

`health-gate` judges a captured status document. It accepts only
`http://127.0.0.1:8080/api/status` with `ok`, `runtime_ready`, and `stt_ready`
true, `stopping` false, `microphone` `connected`, `voice` `WAITING_WAKE`,
`health.overall` `ready`, and `admin`, `configuration`, `runtime`, `llm`,
`stt`, and `wake` ready. It does not open a socket and does not prove which
PID or executable produced the document. The owner compares PID,
`/proc/PID/exe`, that file's SHA256, the `maps` library paths, and
`readlink current` separately.

## Permissions and systemd

The tool runs as the invoking user and cannot `chown`. After `install-release`
into the production prefix:

- directories `0755`, data files `0644`, `firefly-demo`, `firefly-config`, `python`, and shared objects `0755`;
- owner should `chown -R root:root` the prefix so `User=firefly` cannot modify releases or models;
- `/var/lib/firefly/store` stays the existing private directory, mode `0700`, owned by `firefly`;
- the `firefly` account must remain in group `audio` for the existing ALSA devices. The unit does not add `SupplementaryGroups`.

The unit keeps one `firefly-demo` main process, `Type=simple`, `User=firefly`,
`Group=firefly`, `Restart=on-failure`, `RestartSec=5`, `TimeoutStopSec=45`,
`KillMode=mixed`, `KillSignal=SIGTERM`, start limit 5 in 120 seconds, journald
on stdout and stderr, `NoNewPrivileges=true`, `PrivateTmp=true`,
`ProtectSystem=strict`, `ProtectHome=read-only`, and
`ReadWritePaths=/var/lib/firefly/store`. Admin is `127.0.0.1:8080` only.
Voice, wake, STT, VAD, capture, and playback arguments match the accepted
M18.1 values, plus the explicit Admin model path. There is no shell supervisor.

`ProtectSystem=strict` leaves `/opt` and `/usr/bin/python3` readable. The only
writable application path is the ConfigStore. `PrivateTmp` does not hide
`/dev/snd`.

Checked-in unit: `packaging/firefly-voice-assistant.service`. It is the
generator output for `/opt/firefly/voice-assistant`. Nothing in this milestone
copies it to `/etc/systemd/system`.

## Previous installation

The running service still uses the checkout build, the SDK library path, and
`/var/lib/firefly/store`. This worktree does not modify that unit, that
binary, or that store. A later cutover installs one new release beside any
future releases, backs up the checkout unit, and only then replaces `current`.
The store format is still schema 2, which this release can read, so the
cutover does not migrate or rewrite configuration. The first rollback restores
the saved checkout unit and leaves the new prefix on disk. Later rollbacks
move `current` between releases whose manifest can read schema 1 or 2.

## Build inputs that stay off the device at runtime

Configure still reads, and does not write, the RKLLM SDK, the sherpa bundle,
and the production checkout's Admin `dist/` and wake site-packages. Those are
compile-time or staging inputs. After `cmake --install` and staging, the
release RUNPATH, unit, manifest, and wake wrapper do not name the checkout or
the SDK.

## Validation

Commands used in this worktree, without touching the running service:

```sh
cmake -S . -B build \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DFIREFLY_BUILD_ASSISTANT=ON -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_VOICE=ON \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_ADMIN_DIST_DIR=/home/firefly/FireflyVoiceAssistantLinux/src/admin/frontend/dist \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python
cmake --build build -j2
ctest --test-dir build -R 'ProductionPackaging|ProductionStartupContract|ProductionStartup|HealthReliability' --output-on-failure
ctest --test-dir build --output-on-failure -j1
git diff --check
```

Automated packaging uses fixture models. It still links and copies the real
RKLLM, sherpa, and ONNX Runtime libraries, the real Amina model, and the real
wake site-packages, and it checks RUNPATH with `readelf` and `ldd`.
Real multi-gigabyte staging and owner NPU/voice smoke were not executed.
Dirty-tree provenance and insufficient storage block production acceptance.

Grok's reported results before the Codex takeover, retained as history:

- Focused CTest on 2026-10-10 after the second review: **11/11 PASS**, 91.65 s.
  `ProductionPackaging` 38.57 s (21 cases), `ProductionStartupContract`,
  `ProductionStartup`, `HealthReliability`, `AdminNetworkSecurity`, and the six
  `Config*` suites.
- Full CTest: **75/77** on that pass, 336.56 s. `WakeVoiceGating` failed with
  `Result emitted after stop` in 0.03 s, and `AdminSetupV2Api` returned
  `409 setup_busy`. Both passed on an immediate rerun (0.03 s and 1.30 s).
  Those isolated reruns did not establish a clean regression gate.
  The configuration suites and packaging suite
  passed inside the full run.
- `git diff --check`: clean.
- `systemd-analyze verify packaging/firefly-voice-assistant.service` exits 1.
  The Firefly line is the expected missing install path
  `/opt/firefly/voice-assistant/current/bin/firefly-demo`. Foreign warnings
  remain for `netplan-ovs-cleanup.service` and `rkaiq_3A.service`. Any other
  `firefly-voice-assistant.service:` line would be a unit defect.
  `classify_systemd_analyze` separates those classes. The exit code is not
  discarded.

Codex independently reproduced the original wake shutdown failure with
delayed stop observation and the original setup API failure with delayed
fsync. The shutdown mock now waits for actual stop; setup tests use separate
log read handles and bounded retries only for the documented transient
`setup_busy` heartbeat/resume responses. Production wake and setup code are
unchanged. The first takeover full run was **76/77**, revealing a third
test-harness timing defect in `SeamlessWake`: its synthetic audio clock
advanced 32 ms while waiting only 1 ms for a worker. A delayed mock probe
reproduced the failure. The corrected harness paces pending probe frames;
the original 256–512 ms bound and safety assertions remain unchanged.
All three affected suites subsequently passed **50 runs each (150/150)**
in 146.31 s. Final serial verification on 2026-10-10:

- Focused gate: **14/14 PASS**, 110.30 s; packaging **45/45 cases**,
  54.69 s, all six configuration suites, startup, health/network security,
  and the three affected timing suites.
- Complete CTest: **77/77 PASS**, 369.16 s. All configuration suites and
  `AdminV2Chromium`, `AdminManualChromium`, `AdminSetupV2Chromium` passed.
  Packaging passed again with 45 cases (54.94 s).
- Python AST checks, 23 documented shell blocks, three embedded Python
  snippets, launcher/wrapper syntax, and `git diff --check`: PASS.
- `systemd-analyze verify`: exit 1 for the expected absent install executable
  and the two documented foreign diagnostics; zero Firefly unit defects.
- Read-only production checks: PID `292014`, active/running, unchanged
  checkout argv and start time `2026-10-09 15:13:11 UTC`; strict health gate PASS.

Builds ran serially in the isolated worktree. Tests used temporary fixtures;
no real multi-gigabyte model staging, NPU/voice smoke, production writes,
service control, commit, push, PR, merge, or cutover occurred.

The packaging suite now covers 45 cases, including independently packaged
maintenance tools, dirty/stale identities, exact-byte manifest checksums,
root read-only store audit in an exclusively owned temporary fixture,
ownership mismatches, publication/backup races, and failure cleanup. The
root fixture uses passwordless sudo when the test runner is not root; it
never operates on the production store. Syntax checks inspect runbook shell
and embedded Python without executing its privileged commands.

## Known limitations

- `/usr/bin/python3` 3.10 remains a host package. The release does not embed a Python interpreter.
- Recreating the wake packages requires the existing site-packages tree. There is no local wheelhouse, and staging must not download one.
- RKLLM runtime and models are same-device staging inputs. This milestone does not produce a redistributable archive.
- The sherpa bundle on disk has no LICENSE file. Off-device copies need the upstream notices.
- The staging user owns the review tree until the owner changes ownership to root.
- `firefly-admin`'s separate worker does not take `--admin-model`; setup there fails closed. The appliance path is `firefly-demo`.
- Developer NPU harnesses still name the SDK paths at their call sites. They are not in the release.
- Build-tree `build/firefly-demo` still has an absolute RUNPATH. The relocatable binary is the installed one.
- Activation never restarts systemd. Health after restart is an owner step.
- Rollback across a future schema other than 1 or 2 is refused.
- Vendor hangs and the 30 second inference deadline are unchanged. `TimeoutStopSec=45` is still not a bound on a stuck RKLLM call.
- The first production stage needs about 9.3 GiB free; stage-then-install needs about 18.6 GiB plus headroom on one filesystem. Owner production installation is not ready.
- `--production` also requires a clean commit. This worktree is dirty, and it was not committed.
- The health gate reads a captured status document. It does not identify the process.

- Checksums and build identities detect corruption and accidental stale builds; this bundle is not cryptographically signed against a malicious publisher.
- Atomic replacements protect ordinary reported failures. They do not make release markers and unit changes one power-loss transaction; keep the saved unit and recorded release IDs for manual recovery.
