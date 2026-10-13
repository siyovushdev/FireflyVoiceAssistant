# Release procedure audit

This is an operator-only offline audit. Production was not stopped or deployed.
The existing installed SafetyGate exposure remains until a separately approved
hotfix deployment succeeds.

- [Incident](incident.json): exact extra assertion, rationale, original file
  hashes, invocation excerpt references and journal timeline. The first
  systemctl response is unavailable; its immediate failure cause is unknown.
- [Verification](verification.json): tested source fingerprints, unchanged
  controller/helper/launcher/SafetyGate, counts and real systemd case summaries.
- [Test output](test-results.json): complete selected logs with original paths
  and SHA256.
- [Production comparison](production-unchanged.json): read-only PID, release,
  readiness, sole NPU/microphone ownership and exact ConfigStore metadata checks.
- [Raw file-only evidence](file-only-raw.json.gz): lossless JSON records containing
  original paths, per-file SHA256 and complete text. Includes frozen plans,
  typed systemd supervision properties, command responses, journals, results
  and simulated release switches. Runtime boundaries are explicitly FILE_ONLY.

No credentials, production configuration contents, models or full agent sessions
are included. Originals, including early test-only harness failures, remain in
the private audit paths; only completed final runs count toward acceptance.

```sh
sha256sum -c SHA256SUMS
python3 - <<'PY'
import gzip, hashlib, json
verification = json.load(open('verification.json'))
raw = gzip.decompress(open('file-only-raw.json.gz', 'rb').read())
assert hashlib.sha256(raw).hexdigest() == verification['raw_bundle']['decompressed_sha256']
for record in json.loads(raw):
    assert hashlib.sha256(record['text'].encode()).hexdigest() == record['sha256']
print('Lossless original-file hashes verified')
PY
```

Use the [fixed procedure](../../release-procedure.md) for a future separately
authorized release. Do not reconstruct additional assertions from this audit.
