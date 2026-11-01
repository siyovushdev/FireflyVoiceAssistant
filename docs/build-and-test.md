# Build and isolated verification

## PC CPU tests (native host, not a release)

Requirements: Linux, C++17, CMake ≥3.16, Python 3, Node ≥22.12.0. Frontend versions are pinned by `src/admin/frontend/package-lock.json`. `npm ci` may fetch those development packages on the PC; `npm ci --offline` requires a populated cache. There is no model download. CMake never runs npm or fetches assets.

```sh
cd src/admin/frontend
npm ci
npm run typecheck
npm test
npm run build
cd ../../..
env -u DISPLAY cmake -S . -B build/host \
  -DFIREFLY_HOST_OFFLINE_TESTS=ON \
  -DFIREFLY_BUILD_ASSISTANT=OFF -DFIREFLY_BUILD_VOICE=OFF \
  -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j2
env -u FIREFLY_OWNER_ACCEPTED -u DISPLAY ctest --test-dir build/host --output-on-failure
```

Host mode requires BUILD_TESTING and forbids real assistant/voice builds. It uses real C++ core, ConfigStore, SafetyGate, HTTP and Admin code with test workers/private directories. It does not bypass the architecture check for a release. Set `FIREFLY_BUILD_ADMIN=OFF` only for a smaller core subset; this omits important Admin/HTTP coverage. No x86_64 output can serve as the Firefly production candidate.

This host configuration excludes RKLLM-linked fixtures, native ALSA/Sherpa targets, full `ProductionPackaging` (which needs the native appliance and bundled dependencies), and `AminaStreamingBackend` (which needs the frozen wake Python). Unsetting DISPLAY omits four legacy Chromium CTest registrations; that is NOT_TESTED browser coverage, not a browser PASS. Frontend unit/static checks still run. Real native CTest was historically 85/85 at the installed release; new host results must be reported separately. The 2026-10-16 audit additionally built the existing unified HTTP fixture directly with the preserved RKLLM header and FakeRkllmRuntime on the PC; its saved command is private evidence, not a production build.

Applicable operator tests are ordinary CPU/private-file tests:

```sh
ctest --test-dir build/host -R '^(ReleaseRecovery|DeploymentLauncher|ReleaseRunner|InstalledModelReferences|AdminV2Packaging)$' --output-on-failure
python3 tests/M19HardwareRecoveryTests.py
python3 tests/M19HardwareStartTests.py
python3 tests/M19PcTransportTests.py
python3 tests/M19AdminStressTests.py
```

Read each harness before executing it. The Python unit-test entry points above mock systemd/hardware; do not use their fixture/CLI subcommands to start a window. `ReleaseRunnerSystemdTests.py` and real systemd fixtures are separately scoped operator integration tests, not needed for a documentation cleanup on production. `m19-hardware-start.py trial`, model-backed speech/PCM replay, acoustic `aplay`, and benchmark acceptances are not offline unit tests. Never infer safety from a filename alone.

## Native aarch64 production build

Use a fresh clean checkout at the reviewed exact commit, with existing matching pinned dependencies. The proven scheme builds natively on Firefly, serially under bounded CPU/memory/I/O limits, without executing the assistant or touching NPU/microphone. Do not build when the board's memory/disk budget is inadequate. The control PC cannot directly produce an aarch64 release with its native compiler.

The installed c7 build used these dependency locations (preserve them; revalidate for future work):

```sh
# On the target, only in an approved isolated build checkout.
# Build frontend first as above, or supply a verified same-source dist.
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
env -u DISPLAY cmake -S . -B build \
  -DFIREFLY_HOST_OFFLINE_TESTS=OFF \
  -DFIREFLY_BUILD_ASSISTANT=ON -DFIREFLY_BUILD_VOICE=ON \
  -DFIREFLY_BUILD_ADMIN=ON -DFIREFLY_BUILD_CONFIG_TOOLS=ON \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python \
  -DFIREFLY_WAKE_MODEL_REFERENCE= \
  -DFIREFLY_ADMIN_DIST_DIR="$PWD/src/admin/frontend/dist"
cmake --build build --parallel 1
```

The installed c7 timestamp was pinned with `SOURCE_DATE_EPOCH=1792064905`; exact reproduction must also retain toolchain, canonical paths, flags, dist fingerprint and strip/install procedure. Its cached dist came from the preserved `FireflyVoiceAssistantLinux-m19-single-json` checkout and matched the reviewed assets hash. A new checkout should build its own same-source dist and verify its hash, not silently reuse an arbitrary old directory. CMake source fingerprints include canonical paths; matching Git content alone is not a matching build fingerprint.

The existing release-runner performs native CTest/frontend and mandatory recovery gates during authorized preparation. Review which suites are registered and their dependencies; no fresh hardware test is implied. For staged production artifacts, compare `firefly-demo --build-identity`, config-tool identity, source fingerprint, ELF architecture, dependencies, manifest and embedded assets. Follow [release procedure](release-procedure.md) and [no-copy model contract](m19-production-preparation.md); do not manually switch the release symlink.

## Config compatibility without mutation

Copy only the required configuration into a protected PC evidence directory, then run:

```sh
build/host/firefly-config validate --file /absolute/private-copy/configuration.json
```

This validates schema and content hash without opening the live store or executing devices. `audit --store` is read-only; `init`, `save`, `restore`, `reconcile` and runtime Apply are not substitutes for a read-only check. Diagnostic fields such as `production_activated:false` describe the tool's operation, not the state of the live service.

The [CLI reference](runtime-cli.md), [M4 voice checkpoint](live-voice.md), and old milestone build commands are specialist/historical material. They are not current production launch instructions.
