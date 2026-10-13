# Fresh master-integrated M19 proof

Tested clean code `dd1ffa204a5757f32a13c7943a6ed05df68b0f49`, master parent
`818157ec0eef7eb7cb458983db2022af7f7c78ea`. Subsequent commit is docs-only.

[Integration proof](integration-proof.json) records exact build identity,
application/worker SHA256, synthetic voice routes, HTTP counts, knowledge changes,
rejection results and read-only production comparison. [CTest](ctest.log)
passed 87/87; [frontend](frontend-tests.log) passed 41/41 plus private typecheck/build.
[Worker linkage](worker-ldd.log) proves no model/audio runtime dependencies.

[Full Chromium results](chromium-results.json.gz) and
[full grounding matrix](grounding-matrix.json.gz) are lossless gzip copies.
Decompress to inspect `/results/<index>` or `/cases/<index>` in the original JSON.
AI is **SCRIPTED**, voice is **SYNTHETIC PCM/KWS/VAD/STT**. C++ backend, Vue,
source=voice execution dispatch and independent localhost HTTP observation are
real. Do not credit this as new Qwen, reSpeaker or physical-lamp acceptance.

Cabinet desktop and no-zone mobile each have one final Apply, 2 POST and 7
explicit GET, including observation failures/restart. A rejected owner alias
profile has zero Apply/HTTP. The 450-case matrix has 164 accepted/correct POSTs
and 286 rejected/zero HTTP. There are no browser exceptions/external requests.

Screenshots show [original scripted forms](cabinet-desktop-02-proposal.png),
[HTTP Review](cabinet-desktop-06b-http-review.png),
[ON Confirmed](cabinet-desktop-08-on-confirmed.png),
[OFF Confirmed](cabinet-desktop-10-off-confirmed.png), and
[mobile/no-zone Confirmed](no-zone-mobile-08-on-confirmed.png).

[Provenance](provenance.json) gives each private original and SHA256;
[SHA256SUMS](SHA256SUMS) checks the publication. Fixture artifacts contain no
credentials or production configuration. Private production manifests stay private.
Originals and all screenshots/logs are under
`/home/firefly/firefly-m19-integrated-20261013T052104Z/`; earlier evidence is
preserved. [The review report](../../../m19-integration-review.md) separates
working offline MVP from the remaining genuine Qwen/physical voice gate.
