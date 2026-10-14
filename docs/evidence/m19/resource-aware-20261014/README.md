# M19 bounded acceptance evidence — 2026-10-14

Overall instrumented result: **PARTIAL_PASS**. See `acceptance.json` for the exact
boundary. Real Admin4096/AI/private Apply/restore and real-microphone ON→POST→GET
passed. Owner-reported ON/OFF is recorded separately from instrumented acceptance.
Instrumented OFF and full-context stress are not claimed.

| File | Evidence |
| --- | --- |
| `original-ai-proposal.json` | Original real Admin proposal, not hand-corrected |
| `voice-on.json` | Actual human STT, safety and private execution evidence |
| `mock-requests.jsonl` | One ON POST and two GETs; extra GET was harmless |
| `memory-phases.json` | Phase records extracted from the real test log |
| `build-identity.json` | Tested ELF, embedded old HEAD and source/assets hashes |
| `source-manifest.json` | 197 canonical-path source file hashes |
| `offline-browser.json` | Final offline browser proof, one Apply, POST2/GET2 |
| `offline-validation.json` | 14 native / 36 Python, grounding and GET regression |
| `recovery.json` | Original production restoration and identity preservation |
| `closure-preservation.json` | Fresh read-only production/source/resource check |
| `independent-review.json` | Independent closure review and retained limits |
| `sha256.json` | Digest of each other file in this bundle |

These are selected original records or explicitly scoped summaries of retained
raw archives. Source-artifact digests bind summaries to the full proposal,
controller events, test log and result. They do not turn missing coverage into
PASS. Full archives remain on the control PC under
`evidence/hardware-owner-budget140-20261014/` and
`evidence/nonvoice-final-20261014/`. No recordings, model files, lease tokens or
SSH credentials are included.

For product-source identity, sort by canonical absolute filename, using the Firefly root in
`source-manifest.json`, encode each record as `absolute-path:sha256` followed by
a newline, then
SHA-256 the concatenated UTF-8 text. A local checkout must rebase only
its path prefix to that canonical root. New commit IDs are not the tested
executable's embedded dirty HEAD. Hardware memory metrics overlap and must not
be added; 1 Hz sampling may miss transients.
