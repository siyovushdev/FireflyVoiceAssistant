# M19 REAL QWEN TEXT — immutable audit handoff

Handoff status: **READY_FOR_CHIEF_ENGINEER_EVIDENCE_REVIEW**.

The **tested code is `8a6eb084dba4aecb19af2894f5b26c6cf3ca69e0`**. This directory
is published in a later **docs-only commit**; that publication commit was not the
runtime tested on the board. The [Chief Engineer checkpoint of 2026-10-12](https://github.com/siyovushdev/FireflyVoiceAssistant/pull/15#issuecomment-6117504035) requests
independent inspection of these captured artifacts. This bundle supports that
inspection and does not claim the reviewer has already signed them off.

No new runtime, model inference, service stop/restart or device test was performed
to prepare this handoff. The original run was one owner-authorized **text-only**
window with a real C++ Admin/runtime, RKLLM 1.3.0 / Qwen3-4B W8A8 and an isolated
localhost mock. It reused previously reviewed speech aliases; it was **not** a
new AI Setup, Chromium workflow, acoustic wake/STT or physical voice acceptance.

## Files and provenance

- [audit-bundle.json](audit-bundle.json): source/binary/assets identity, original
  inputs and responses, grounding spans, index diagnostics, SafetyGate decisions,
  actual inference metrics/raw output, HTTP/state evidence and restoration proof.
- [http-observer.jsonl](http-observer.jsonl): verbatim complete mock log, six lines.
- [supervisor.log.txt](supervisor.log.txt): verbatim stop/launch/cleanup/restore
  chronology from the independently running watchdog.
- [text-client-as-run.txt](text-client-as-run.txt): verbatim captured client,
  provided as documentation. Its historical “Prepared only” header is preserved;
  the completed run and response evidence establish its later execution. The
  client records per-command request-log deltas and requests RESTORE in `finally`.
- [original-evidence-sha256.json](original-evidence-sha256.json): verbatim original
  digest inventory, **214 files**, including raw reports and protected backups.
  All 214 digests were checked again during handoff without changing originals.
- [SHA256SUMS](SHA256SUMS): integrity of the published files, excluding itself.

`audit-bundle.json.sources` records the SHA256 and byte size of every source used
in an excerpt/attachment, including the digest inventory itself. Source names
are relative to the original run root
`firefly-m19-real-qwen-text-20261012T035138Z` identified in the PR checkpoint.
Each excerpt carries an exact JSON pointer plus optional field allowlist, a
one-based inclusive line range, or a zero-based whitespace-token index. Values
are unchanged; omitted fields are documented rather than silently rewritten.
Verbatim attachments retain the original byte digest. Derived counts/table rows
refer to the underlying excerpt IDs and can be recomputed from them.

The JSON projections omit private checkout/home paths, production configuration
contents, setup drafts, model binaries and unrelated process details. They retain
only the test's localhost contract and bodies, configured test names, raw model
outputs, release identity, hashes, numeric ownership/modes, PIDs and timestamps.
No credentials, cookies, authorization headers or secret values are published.
Hashes bind excerpts to preserved originals; they are **not** a third-party
attestation of how the on-device capture was produced.

## What the original run records

Clean build: `dirty=false`, built `2026-10-12T03:52:17Z`:

| Identity | SHA256 |
| --- | --- |
| Source fingerprint | `113fd33c89d82ab265b9c0d81d29137f0ea49b11180dc2431d5467e655ce1ccf` |
| Embedded Admin assets | `fd3cbf4dd6403204f35a8759c8678282d37958854d7654117c5cdc29a01110f9` |
| Tested executable | `8ec1ddb767917f58e03f87720a28010f2aaea7c14da035943ebdd9fd6ab11c0c` |

The vendor log records RKLLM 1.3.0, RK3588, W8A8, three NPU cores and the fixed
Qwen3-4B model pathname. Two **command inference** log lines, correlated with
the API metrics, distinguish actual inference from initialization/bootstrap:

| Case | Original text | Raw | Prefill ms | Generation ms | Output tokens | SafetyGate |
| --- | --- | --- | ---: | ---: | ---: | --- |
| 03 | включи лампу в неизвестной комнате | `[]` | 1235.62 | 266.374 | 1 | REJECT |
| 08 | будь добр включи лампу в кабинете | `[]` | 1124.44 | 271.496 | 1 | PASS |

Case 03 has only device grounding, with `неизвестной`, `комнате` uncovered. The
decoded candidate still names the configured lamp/action, but SafetyGate returns
Unsupported, execution is null and its HTTP delta is empty. Case 08 preserves
the configured location/device, permits the polite prefix and executes the
correct ON action. **INDEX_EXACT cases have zero inference tokens/metrics and
are not counted as successful Qwen calls.**

| Case | Original text | Route | Safety | POST / separate GET |
| --- | --- | --- | --- | --- |
| 01 | включи лампу в кабинете | INDEX_EXACT | PASS | 1 / 1, Confirmed true |
| 02 | выключи лампу в кабинете | INDEX_EXACT | PASS | 1 / 1, Confirmed false |
| 03 | включи лампу в неизвестной комнате | LLM_FALLBACK | REJECT | 0 / 0 |
| 04 | включи лампу в спальне | INDEX_AMBIGUOUS | NO_ACTION | 0 / 0 |
| 05 | включи лампу в кабинете и в спальне | INDEX_AMBIGUOUS | NO_ACTION | 0 / 0 |
| 06 | не включай лампу в кабинете | GROUNDING_REJECTED | NO_ACTION | 0 / 0 |
| 07 | какая погода | GROUNDING_REJECTED | NO_ACTION | 0 / 0 |
| 08 | будь добр включи лампу в кабинете | LLM_FALLBACK | PASS | 1 / 1, Confirmed true |
| 09 | включи лампу | INDEX_AMBIGUOUS | NO_ACTION | 0 / 0 |
| 10 | включи лампу в неизвестной комнате | INDEX_AMBIGUOUS | NO_ACTION | 0 / 0 |
| 11 | включи лампу в кабинете | INDEX_AMBIGUOUS | NO_ACTION | 0 / 0 |

Generation 1 contains the lamp in Кабинет and the configured comparison room
Спальня. Two explicit isolated Test Apply calls introduce a second lamp in
Спальня for cases 09/10 (generation 2), then place both lamps in Кабинет for
case 11 (generation 3). The bundle retains each active test configuration and
generation/hash. The completed client checks Apply HTTP 200 and the subsequent
API responses show the active generations; individual Apply response bodies were
not captured and are not invented here.

All three successful actions target `device_lampa`, with the correct ON/OFF
action, one `POST /relay/1` and exact `{"on":true/false}` body. Each explicit poll
causes a separate `GET /relay/1` and records `knowledge=confirmed`, correct value,
`stale=false` and observation timestamp. The captured execution history has
exactly three entries, source **`admin-production`**. This source describes the
execution intent of the test API in the **isolated TEST store**; it is neither
production-service execution nor `source=voice`.

The complete mock log has only **3 POST + 3 GET**. Each of the eight rejected
cases has Unsupported, null execution and an empty request-log delta. The
captured client also asserts that rejection adds no execution-history entry.
The mock JSONL has no per-request timestamps; attribution to individual commands
comes from the client's saved before/after deltas, not fabricated HTTP timestamps.
NO_ACTION means the request was rejected before an executable SafetyGate
candidate existed; it must not be relabeled as SafetyGate PASS or REJECT.

## Production restoration

The watchdog log records production stop at `03:54:48.126876Z`, candidate/mock
cleanup at `03:55:18–20Z`, and verified restoration at `03:56:08.429166Z`: about
**80.3 seconds**, below the 600-second authorization. The later separate
on-device read-only audit is timestamped `03:57:00.024417Z`. “Separate” here
means an additional check outside the watchdog, **not** Chief Engineer signoff.

Before PID `217184`, restored PID `337465`; restored source is still
`dc7b142e4acae74d178a2583cdc1568ec4de8ee8`, release
`wake-beep-dc7b142e-20261011`. The retained status records READY, STT ready,
microphone connected and WAITING_WAKE. The additional audit records sole
NPU/microphone owner `[337465]`, no candidate/mock/client/watchdog processes and
no LISTEN servers on 18080/18765.

The before/after manifests retain every production ConfigStore path, SHA256,
size, mode, UID and GID. Files remain 0600, directories 0700; active/stored
revision 2 hash stays
`688690e892d2f59413cdc2f7d81f1e129ab9ac0dd247f84b2898d74c3c3cdd18`.
The service republishes identical LKG bytes at startup, so LKG/directory mtimes
change; those exact changes are retained rather than called byte-for-byte
metadata equality. The full protected backup remains unchanged. The recorded
systemd unit digest is
`77334f69759d06ae07c9b052511544c6ceefbaa5e7e1e2434ac749e839045b39`.
Watchdog excerpts show the actual store comparison, `fuser` ownership checks,
bounded owned-process cleanup and mandatory systemctl restoration logic.

The installed production release **has not received the grounding fix**. These
records establish restoration to the old release, not its safety for actuators.
PR #15 remains Draft; Issues #16/#17 remain open. Independent evidence review,
separate audio acceptance and release approval remain outstanding.

## Reproduce the evidence comparison offline

From this directory, `sha256sum -c SHA256SUMS` checks the published bundle. For a
reviewer with read-only access to the originals, this comparison checks all 214
original digests and every selected value against its hashed source. It does not
connect to an API, load models, open audio devices or control services:

```sh
python3 - /path/to/firefly-m19-real-qwen-text-20261012T035138Z <<'PY'
import hashlib, json, sys
from pathlib import Path
root = Path(sys.argv[1])
bundle = json.loads(Path('audit-bundle.json').read_text())
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
inventory = json.loads(Path('original-evidence-sha256.json').read_text())
for name, expected in inventory.items():
    assert digest(root / name) == expected, name
for name, expected in bundle['sources'].items():
    assert digest(root / name) == expected['sha256'], name
    assert (root / name).stat().st_size == expected['bytes'], name
for record in bundle['excerpts']:
    path, selection = root / record['source'], record['selection']
    if selection['kind'] == 'json_pointer':
        value = json.loads(path.read_text())
        for token in selection['pointer'].split('/')[1:]:
            token = token.replace('~1', '/').replace('~0', '~')
            value = value[int(token)] if isinstance(value, list) else value[token]
        if 'fields' in selection:
            value = {key: value[key] for key in selection['fields']}
    elif selection['kind'] == 'text_lines':
        value = ''.join(path.read_text().splitlines(keepends=True)[
            selection['first'] - 1:selection['last']])
    elif selection['kind'] == 'whitespace_token':
        value = path.read_text().split()[selection['index']]
    else:
        raise ValueError(selection)
    assert value == record['value'], record['id']
for record in bundle['attachments']:
    assert digest(Path(record['published_file'])) == record['sha256']
    assert Path(record['published_file']).read_bytes() == (root / record['source']).read_bytes()
print('PASS original digests, exact excerpts and verbatim attachments')
PY
```
