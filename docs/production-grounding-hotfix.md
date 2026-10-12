# Production original-request grounding hotfix

This hotfix addresses [Issue #17](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/17).
With a lamp configured in Cabinet, `включи лампу в неизвестной комнате` must
produce no device request, even if inference proposes the registered lamp ID.
The installed implementation accepted that proposal while silently losing the
unknown-location constraint.

## Provenance and scope

The branch starts at master `5fed423`, whose complete Git tree is identical to
installed source `dc7b142e`. The clean installed binary identifies that source;
its source fingerprint also matches the original release worktree. An offline
build of that SafetyGate reproduced `INDEX_MISS -> LLM_FALLBACK -> PASS -> POST`
against a disposable localhost mock. Inference was scripted and adversarial;
the installed runtime was never sent the unsafe command.

The only runtime implementation change is `src/SafetyGate.cpp`, copied exactly
from corrected candidate `8a6eb08`. CMake registers the related HTTP regression.
The existing tests are updated to reject unsupported source/time constraints
and unreviewed operation typos instead of expecting the gate to ignore them.

The shared gate checks the complete original request against the selected
configured target, action, room and already validated argument values. Unknown,
conflicting or ambiguous locations and remaining unsupported constraints reject
the candidate; the gate cannot repair it. Quoted scalar data remains distinct
from an unquoted location. INDEX_EXACT and fallback use the same authorization
boundary. Valid polite fallback and declared Russian forms remain supported.

No AI Setup, Web Admin, ConfigStore/schema, executor, command protocol, RKLLM
prompt/settings, voice, wake, ALSA, VAD, STT, BEEP, unit or release-pointer changes
are included. Fresh frontend assets have the installed fingerprint:
`6ec619a8145689b1ce16bcc9b246f3d9e7fad779be44b0cfde689a7c94b73e27`.
The private copy of the active production profile validates identically under
the installed and candidate configuration tools; no Apply or outbound request
is part of that check. Production remains on its original running release.

## Automated evidence

See [verification.json](evidence/production-grounding-hotfix/verification.json)
and its linked raw regression evidence. All model outputs in these new tests
are scripted. The voice-seam recognizer is synthetic and tests authorization
and execution routing, not real microphone/wake/STT recognition. No new Qwen
inference or hardware acceptance is claimed. Wake investigation remains deferred
under [Issue #18](https://github.com/siyovushdev/FireflyVoiceAssistant/issues/18).

The 450-case matrix covers both command protocols and direct gate, parser/engine,
runtime worker, text API, guarded device API and synthetic voice entry points.
It includes unknown and conflicting rooms, duplicate device names, devices
without zones, numeric device names, malicious aliases, forged target IDs,
Russian inflections and valid ON/OFF/polite commands. The independent localhost
observer requires zero HTTP for every rejection and exactly one correctly
addressed POST/body for every permitted command. Full CTest also covers existing
HTTP observation/state confirmation, voice lifecycle and packaging contracts.

Results: **81/81 CTest**, **40/40 frontend tests**, typecheck and frontend build
PASS; **450/450 authorization cases**, including 164 permitted commands with
164 observed POSTs and 286 rejections with zero outbound HTTP. The full raw
baseline and fixed reports are published as lossless gzip. After decompression,
`/cases/<index>` identifies each case in its original JSON; `verification.json`
records both original and published SHA256, and `SHA256SUMS` checks the bundle.
The tested working-tree build identity is recorded explicitly, including its
dirty flag. The PR records the final clean commit identity and verifies an
identical source fingerprint after rebuilding metadata.

To reproduce on Linux/aarch64 with the existing local dependencies:

```sh
cd src/admin/frontend
npm run typecheck
npm test
npm run build
cd ../../..
cmake -S . -B build -DFIREFLY_BUILD_CONFIG_TOOLS=ON -DFIREFLY_BUILD_VOICE=ON \
  -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64 \
  -DSHERPA_ONNX_ROOT=/home/firefly/FireflyVoiceAssistantLinux/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu \
  -DSHERPA_ONNX_INCLUDE_DIR=/home/firefly/FireflyVoiceAssistantLinux/build/stt/vad-ab \
  -DFIREFLY_WAKE_PYTHON=/home/firefly/FireflyVoiceAssistantLinux/build/wake-venv/bin/python \
  -DFIREFLY_ADMIN_DIST_DIR="$PWD/src/admin/frontend/dist"
cmake --build build -j2
ctest --test-dir build --output-on-failure -j2
```

Use existing pinned dependencies; no model/dependency download is needed.
For raw matrix evidence set `FIREFLY_GROUNDING_EVIDENCE` to a new private JSON
output path when running CTest. The regression uses ephemeral loopback ports
and disposable test stores; it never opens the production ConfigStore.

## Proposed update and rollback — not executed

1. Obtain independent Chief Engineer approval of this Draft PR and separate
   owner authorization for deployment/downtime. This preparation authorizes
   neither merge nor deployment.
2. Build the approved clean commit with native voice enabled. Check build
   identity, source/asset fingerprints and the complete automated suites. Stage
   and verify a new release using the existing
   [M18.6 packaging procedure](m18-relocatable-production-packaging.md), in a
   private staging destination first. Preserve the three libraries, shared
   models, wake interpreter/model/worker, frontend assets, unit and CLI options.
   Never copy new models over the existing shared files.
3. Immediately before any approved cutover, verify the then-current release,
   production health/PID and active/stored revision/hash; do not assume today's
   baseline is still current. Keep a protected, rights-preserving full store
   backup and per-file SHA256 manifest. Keep
   `wake-beep-dc7b142e-20261011` and its rollback materials intact. Activate an
   independently supervised recovery deadline before stopping anything.
4. In the separately approved exclusive window, stop the service once and prove
   NPU/microphone ownership is released. Any candidate acceptance uses an isolated
   store and localhost mock. Do not run two NPU/voice owners and do not issue
   actuator commands through the production profile. Switch only the release
   pointer through the reviewed packaging procedure; keep the existing unit,
   flags, shared models and ConfigStore. Start the single approved service.
5. Verify build identity, READY/WAITING_WAKE, STT/wake readiness, connected
   microphone, sole ownership and unchanged active/stored revision/hash and
   store bytes/permissions. Missing readiness or deadline expiry triggers the
   independent rollback. Do not tune wake/STT or change configuration to pass.
6. Rollback stops only the candidate service/process, confirms resources are
   released, restores the recorded original release pointer and starts the same
   unit. Recheck health, ownership and unchanged store. Do not overwrite the
   ConfigStore as a routine rollback or delete either release/backup.

Rollback restores the old grounding vulnerability as well as the old runtime;
it is operational recovery, not a safety fix. Until an approved hotfix is
installed, production retains the source-level exposure demonstrated here.
The stronger gate deliberately rejects unsupported qualifiers, timing/source
clauses and unreviewed operation typos; that is a possible false-rejection
compatibility change, with no automatic transcript correction or alias migration.
