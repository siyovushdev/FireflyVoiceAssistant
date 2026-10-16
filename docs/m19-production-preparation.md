# M19 production preparation (no activation)

> Dated milestone/design record. Its test results and deployment statements apply to the recorded revision, not today. For current M19 identity, architecture and operating instructions, start at [the production baseline](production-baseline.md) and [README](../README.md). Historical results below are preserved.

Product base: master `4118088eb938e1c616798fce5e08bfac3a794f58`.
Packaging changes are committed locally on a descendant; the approved commit
must be that exact clean HEAD, including packaging, runner and CMake changes.
Build natively on Firefly aarch64, with the existing frozen SDK/header, Sherpa
bundle and wake Python. Build/test in an isolated checkout with DISPLAY unset,
serial compilation, bounded memory and low CPU/I/O priority. No NPU inference
or production stop is part of preparation.

`FIREFLY_WAKE_MODEL_REFERENCE` names the canonical, regular, root-owned frozen
Amina model in `grounding-818157ec-20261013T040322Z`. CMake uses a link in the
build tree and does not install a new model copy. The stage command requires
`--production --reuse-installed-models --wake-reference-release
grounding-818157ec-20261013T040322Z --runtime-prefix /opt/firefly/voice-assistant`.
All seven model inputs must be their exact existing installed `/opt` paths.
The package contains the application, its own libraries/assets/worker/Python,
a pinned Amina link, and an explicit installed-model reference contract.
Models remain unchanged; missing/tampered/aliased models fail closed.
The referenced old release is a protected permanent dependency until a later
explicitly reviewed migration removes the reference. Never delete it.

`install-release` publishes only an inactive immutable release. It does not
switch current/previous-release, change the unit or ConfigStore, or restart
production. Install is restricted to the fixed production prefix for references.
The existing integrity checks still verify both releases, libraries, seed,
voice worker/dependencies, unit contract, model hashes and source/build identity.

Run the existing PC browser harness with `offline-gate --remote-repo` naming
the new clean Firefly checkout. Chromium stays on the PC; backend, private mock
and evidence stay on Firefly. Scripted AI is only an offline fixture. Alternate
checkouts are rejected in prepare-only/trial, preserving hardware-window scope.

The existing release-runner `prepare` entry point requires the exact clean
approved commit and `--approved-admin-assets-sha256` equal to the reviewed M19
assets fingerprint. New ELF, its package manifest, and freshly built frontend
must agree with that pin. Legacy invocations retain old/new equality.
The pin is recorded in the SHA-protected recovery plan. Preparation runs offline
application gates and independent file-only systemd recovery fault fixtures,
backs up ConfigStore with permissions/ownership/timestamps, and verifies current
production. The plan remains unarmed; activation requires new owner permission.
Recovery and forward controllers live on Firefly, independent of SSH/PC, with
240-second verification and 360-second recovery budgets. Only activation arms
the boot-bound watchdog and validates the exact systemd timer before cutover.

M19 acceptance stays PARTIAL_PASS: real Admin Qwen4B ctx4096, AI Setup, Apply
and instrumented microphone ON → POST → GET passed previously. OFF was checked
by the owner; a complete instrumented OFF E2E and ctx4096 stress remain open.
Preparation does not repeat those successful hardware trials or claim new
hardware acceptance, push, merge, tag, or production activation.
