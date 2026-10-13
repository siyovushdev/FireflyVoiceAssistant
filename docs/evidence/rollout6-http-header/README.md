# Rollout #6: authenticate non-actuating Admin diagnostics

Tested code: `e20459f99989b3e890a9aeb1461c5c20df4ec1b0`, based on
approved master `0746bf8a9107a4b66f053682ec4c39d2f633015f`.
The following evidence commit changes documentation only.

The controller omitted `X-Firefly-Admin: 1` on `/api/test-command`.
The fix adds that header alongside JSON Content-Type. Both request flags remain
`execute=false` and `production=false`; server security, SafetyGate, deadlines,
launcher, recovery and release runner are unchanged.

[Actual HTTP evidence](http-diagnostics.json) exercises the exact
`Production.non_actuating_checks()` method through a freshly built C++ Admin,
its existing `admin_fake_worker` IPC fixture and a disposable private ConfigStore.
Only the controller's fixed loopback origin is redirected to the isolated Admin
port **inside the test**. Requests retain the controller's headers/body and real
HTTP responses. The worker uses production RuntimeWorker with **MOCK inference**;
`raw_rkllm` is a historical response-field name, not evidence of real Qwen.
No Qwen/NPU or microphone runtime is started by this acceptance.

Both fixed phrases (`какая погода`, `включи лампу в неизвестной комнате`)
receive **403 without the header**, with the actual required-header error.
The corrected helper receives **200**, `result={"unsupported":true}` and
`execution=null` for both. Its request header/body shape is asserted directly.
The fixture has an applied Cabinet lamp with real HTTP bindings to an isolated
localhost mock. A valid lamp preview verifies target/action readiness without
execution. Execution history stays empty, device state and every ConfigStore
file SHA256 remain unchanged; the mock records **zero HTTP requests of any method**.

[Full CTest](ctest.log) includes the new regression within AdminHttp and the
existing packaging/security/grounding tests. [Operator tests](operator-tests.log),
[frontend tests](frontend-tests.log) and the five existing real systemd
[file-only fixtures](systemd-fixtures.json) are also recorded. The fixture units
use only private files and their own transient services/timers.
Results: **84/84 CTest**, **4/4 AdminHttp**, **40/40 frontend** with typecheck/build,
**17/17 launcher**, **34/34 recovery**, **16/16 runner**, **5/5 systemd fixtures**.

[Verification](verification.json) records exact build identity, unchanged
production PID/release/start time/ConfigStore and sole NPU/microphone ownership.
Downtime is zero. Production remains on `dc7b142e`; the SafetyGate hotfix is
still inactive. No prepare/run, production stop/restart, activation or device
command was performed. A seventh rollout has not been authorized.

[Original evidence check](original-evidence-check.json) verifies all 1214 files
from rollout #6 against its saved SHA256 ledger. The original failure captured
HTTPError 403, **not its response body**. The response bodies published here are
from the new isolated regression. [Provenance](provenance.json) links every
published file/excerpt to its private original and SHA256. Original ConfigStore
contents and private production snapshots are not published. [SHA256SUMS](SHA256SUMS)
checks this bundle; private originals are under
`/home/firefly/firefly-rollout-http-fix-20261012T192201Z/`.
