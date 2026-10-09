# M18.3 — Production API Hardening

Baseline: `514bda7393efb9ff9939f186324009007316d982` (M18.2 merged as PR #7,
owner accepted). Branch: `hardening/production-api`. M18.1 and M18.2 remain
COMPLETE / PASS. M18.3 owner acceptance: **COMPLETE / PASS**.

## Old exposure and narrow removal

The production `Api.cpp` route map registered `POST /api/devices/observe`.
Its handler accepted `target_id` and `properties`, built an `observe_state`
runtime message, and ultimately called `DeviceStateStore::applyObservation`.
That validated property values/types and timestamps, but could not prove their
physical origin. An HTTP client with the normal Admin mutation headers could
manufacture Confirmed state without any device readback. Command HTTP 200 and
caller-supplied JSON are not independent observations.

M18.3 removes both the route registration and its forwarding handler. A valid
Admin HTTP request to that path now returns **404 / `not_found`**, including with
valid simulated properties. It is not an undocumented endpoint or a disabled
handler. There is no replacement route, magic header, query flag or localhost
bypass. As with any unknown route, generic HTTP framing/content-type/origin guards
can reject malformed or unauthorized requests before routing; they are unchanged.

## Audit: public surface, internal seams, tests

| Category | Finding and disposition |
| --- | --- |
| A: production HTTP | `/api/devices/observe` registration and handler removed. `/api/devices/state` remains GET-only and reads memory. `/api/devices/poll` accepts only target ID and expected active hash, then uses configured GET readback. `/api/devices/command` retains guarded admission. |
| A: other routes | Audited the full Api route map and setup dispatch. No generic runtime-message bridge exists. Public handlers fix their runtime method and validate fields; `method=observe_state` or injected properties cannot be smuggled through poll/command. `/api/test-command` preserves Playground/explicit production execution semantics; it does not create Confirmed. Config initial values do not initialize physical state as Confirmed. |
| B: trusted runtime seam | `RuntimeGenerations::observe`, `BoundDeviceExecutor::observe`, `DeviceStateStore::applyObservation` and worker `observe_state` remain unchanged. The worker protocol is a private inherited socketpair (fd 3), or an in-process call in DemoMain, not a public HTTP listener. Retaining this seam does not expose it to HTTP clients. Runtime `observe` (without `_state`) is read-only diagnostics, a separate operation. |
| B: real observation | `poll_state` → active generation/hash check → `BoundDeviceExecutor::poll` → registered HTTP executor → configured GET → validated `StateObservation`. The state store remains the shared evidence owner. |
| C: isolated tests | DeviceStateLifecycle directly injects typed observations to test schema, ordering, reconciliation and stale evidence. Those tests remain valid. The Chromium fixture was the HTTP state-injection caller; it now polls a real local HTTP mock for both Confirmed devices. AdminHttp now expects 404 instead of describing the old route as an available guarded mutation. |
| Frontend | The production POST allowlist and UI never called the removed endpoint. No frontend source, bundle or B4 UX change is needed. |

Only `Api.cpp` changes production behavior. Runtime/transport observation methods,
DeviceStateStore, BindingResolver, HardwareExecutor, ConfigStore, SafetyGate,
guarded command handling, voice and M18.2 recovery remain unchanged.

## Authoritative state flow

1. Cold physical state is Unknown.
2. A successful bound command updates **Predicted**, even if the device returns
   HTTP 200 and a body resembling telemetry. Reachability is separate evidence.
3. Reading `/api/devices/state`, dashboard or status does not perform hardware I/O
   or upgrade a prediction.
4. An explicit `/api/devices/poll` with the active hash makes an independent GET
   against the active observation mapping. Valid mapped values become **Confirmed**.
5. An external device change followed by another poll updates Confirmed to the
   observed value, even when it contradicts the previous prediction.
6. Failed, malformed or unreachable observations do not fabricate Confirmed or
   overwrite property evidence. Existing reachability/staleness rules still apply;
   an earlier Confirmed value is not automatically erased by a later failed poll.

## Automated verification

`ProductionApiHardening` runs real Api/HttpServer, ConfigStore, RuntimeWorker and
HTTP executor code with a private temporary store, fake inference and an independent
local TCP/HTTP device. Four cases cover:

- unknown-route 404/not_found across methods, absent route/POST allowlists and no
  runtime forwarding alias;
- successful POST → Predicted, read-only refresh, independent contradictory GET
  → Confirmed, then another independent state change;
- HTTP error, malformed JSON, wrong type and unreachable device preserving
  Predicted instead of fabricating Confirmed;
- GET-only state, rejected poll properties/method/timestamp injection and stale hash.

It reuses the ManualApi fixture. The existing AdminManualApi cases continue to
verify active hash, expected target/action/arguments, SafetyGate rejection and
setup ownership with zero unexpected device requests. The Chromium fixture now
expects three setup hardware requests (two GETs and one command POST), while all
subsequent read-only UI navigation still performs zero hardware I/O.

Commands, using the existing configured offline build dependencies:

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j2
ctest --test-dir build -R 'ProductionApiHardening|DeviceStateLifecycle|HardwareBindings|Connectivity|AdminHttp|AdminGenerationApi|AdminGuardedCommand|AdminManualApi|UnifiedHttp|ProductionStartup|VoiceRecovery' --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

Results (2026-10-09):

- Configure/build PASS; no compiler warnings in the build log.
- Focused suites: **14/14 PASS, 56.79 s**. New ProductionApiHardening: four cases
  PASS (**5.89 s**). DeviceStateLifecycle, HardwareBindings, ConnectivityConfig,
  ConnectivityHttp, AdminHttp, AdminGenerationApi, AdminGuardedCommand,
  AdminManualApi, UnifiedHttp, ProductionStartupContract, ProductionStartup,
  VoiceRecovery and VoiceRecoveryIntegration all passed.
- Full CTest: **74/74 PASS, 277.53 s**, including AdminV2Chromium (**62.30 s**),
  AdminManualChromium (**46.30 s**) and AdminSetupV2Chromium (**43.31 s**).
- `git diff --check` PASS; untracked files also checked for whitespace errors.
  The corrected isolated owner runbook was separately syntax-checked (shell and
  embedded Python) without executing owner commands.

Logs: `/tmp/firefly-m18-3-configure.log`, `/tmp/firefly-m18-3-build.log`,
`/tmp/firefly-m18-3-focused.log`, `/tmp/firefly-m18-3-ctest.log`.
No owner hardware acceptance, installed service operation or access to
`/var/lib/firefly/store` was performed by these tests.

## Final real owner acceptance — COMPLETE / PASS

The owner completed the following checks on the real appliance. These results are
owner-supplied evidence, separate from the automated tests above.

### Phase A — production read-only baseline — PASS

- Service active; `runtime_ready=true`, `voice=WAITING_WAKE`,
  `microphone=connected`, overall health ready and wake status ready.
- `POST /api/devices/observe` with valid Admin headers returned HTTP **404**,
  error code **not_found**.
- Production revision **1**; configuration hash and `configuration.json` SHA256
  both **`36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`**.
  `retired_ids=[]`; no M18.3 acceptance IDs present.
- Production stopped cleanly before the isolated runtime: `ActiveState=inactive`,
  `MainPID=0`, `Result=success`, `ExecMainStatus=0`, `SHUTDOWN complete` and
  `RKLLM destroyed`. No Firefly runtime processes remained; ports
  **8080 / 18083 / 18765** were free.

### Phase B — isolated state-flow acceptance — PASS

Used a disposable `/tmp` ConfigStore, Admin port **18083**, mock device port
**18765**, the production `firefly-demo` binary and production RKLLM model, with
voice disabled. Production `/var/lib/firefly/store` was untouched.

Initial status: `runtime_ready=true`, overall ready, `voice=disabled`.
`POST /api/devices/observe` returned **404 / not_found**.
`device_fan.power` was `knowledge=unknown`, `value=null`, connectivity unknown,
with **zero hardware/device requests**.

Guarded command **«включи вентилятор в цехе»** returned `ok=true`,
`execution.success=true`, `language_route=INDEX_EXACT`, `safety=PASS`.
Device power became true, knowledge predicted, connectivity reachable. Repeated
`GET /api/devices/state` remained Predicted=true and caused **no hardware GET**.

| Step | State returned by Firefly | Exact accumulated device traffic |
| --- | --- | --- |
| Successful guarded command and repeated state reads | Predicted=true | `POST /relay/1 {"on":true}` |
| Explicit `/api/devices/poll` | Confirmed=true | POST, GET |
| Independent direct device `POST /relay/1 {"on":false}`, before Firefly poll | Cached Confirmed=true | POST, GET, POST |
| Explicit Firefly poll after external change | Confirmed=false; connectivity reachable | POST, GET, POST, GET |

Both polls executed real `GET /relay/1` requests. Confirmed followed independent
device evidence even when it contradicted the prior Firefly prediction/cache.

### Failed/unreachable observation — PASS

After stopping the mock, the next `/api/devices/poll` returned **HTTP 422 /
connection_failure**. Connectivity became unreachable; knowledge remained
confirmed, value remained false, and **both `observed_at_ms` and `updated_at_ms`
were unchanged**. No new device request could occur. Prior successful traffic
remained exactly **POST, GET, POST, GET**. Failed observation fabricated neither
a new Confirmed value nor a new evidence timestamp.

### Isolated shutdown — runbook issue found and corrected

The first cleanup attempted `wait PID` from a different shell and reported:
`wait: pid ... is not a child of this shell`. SIGTERM had been delivered and
`SHUTDOWN complete` was observed, but the first immediate check still found
`firefly-demo` listening on **18083**. Sending SIGTERM or seeing that log alone
was therefore not sufficient evidence of completed process teardown.

Follow-up ownership/process/port verification confirmed the mock PID and isolated
`firefly-demo` PID were gone, ports **8080 / 18083 / 18765** were free, no
Firefly/mock process remained, and production was still stopped. Isolated
acceptance evidence was preserved. This was a **RUNBOOK ISSUE FOUND AND CORRECTED**,
not a product/runtime failure. The corrected procedure below verifies
`/proc/<pid>/cmdline`, signals only the known owned process, polls `kill -0`
semantics until actual disappearance, and checks listeners and remaining processes
before permitting directory removal. It does not rely on cross-shell `wait PID`.

### Phase C — production restoration/integrity — PASS

Production restarted normally: `runtime_ready=true`, `voice=WAITING_WAKE`,
`microphone=connected`, overall health ready, wake status ready, Web Admin HTTP
PASS. Systemd reported **MainPID=196756**, **NRestarts=0**, `ActiveState=active`,
`Result=success`.

Revision remained **1**. The configuration hash and file SHA256 both remained
**`36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`**,
exactly matching the pre-acceptance values. The full configuration was unchanged,
`retired_ids` remained **[]**, and no acceptance IDs were present.
`POST /api/devices/observe` after production restart again returned **404 /
not_found**. Owner acceptance did not mutate or pollute the production ConfigStore.

### Final real voice smoke — PASS

Amina produced `WAKE_DETECTED` → `BEEP_START` → `BEEP_END` →
`LISTENING_COMMAND` → `SPEECH_START` / `SPEECH_END` → `PROCESSING`.
STT transcript was exactly **«какая погода»**. The empty production domain safely
returned `unsupported=true` / `target_not_grounded`, then returned to WAITING_WAKE.

Final status: **MainPID=196756**, **NRestarts=0**, `runtime_ready=true`,
`voice=WAITING_WAKE`, `microphone=connected`,
`last_voice.transcript="какая погода"`, overall ready and wake ready.

### Final M18.3 conclusion

**Owner acceptance COMPLETE / PASS.** The public production state-injection route
is removed; no replacement public Confirmed-injection endpoint was added.
Command success produces Predicted only. Confirmed requires configured independent
device observation; contradictory observations override prior prediction/cache,
and failed observations fabricate no Confirmed evidence. `/api/devices/state`
remains read-only, and guarded command/SafetyGate semantics remain intact.

Production ConfigStore was unchanged by acceptance; voice, Web Admin and runtime
were healthy after restoration. The trusted internal observation seam remains for
legitimate internal/test use within the real polling architecture, without a public
injection route. The cross-shell `wait PID` issue was a runbook defect, not a product
defect. XVF3800 firmware >=2.0.10 remains required; 2.0.10 is directly validated.

## Owner acceptance commands — corrected repeatable procedure

Run these only after the approved M18.3 build is available in `build/firefly-demo`
and deployed to the normal service through the existing operational process.
These are owner commands, not automated acceptance results. Keep XVF3800 firmware
>=2.0.10 (2.0.10 directly validated); this procedure does not replace that prerequisite.

Prefer one Bash session for the checks in all three phases, in order. Cleanup also
works from another shell using the saved directory and PID files below; shell
functions/variables for other checks must be restored separately. Stop on any failed
assertion; do not continue to the next phase. Production configuration is read-only throughout:
**no Save, Apply, import, restore, direct store edits, or copying test configuration
into `/var/lib/firefly/store`**. Restoring a catalog does not undo retired IDs.
Do not change service units or perform USB reset / `xvf_host` operations.

Phase A records production evidence and stops production. Phase B uses the real
production executable with a disposable store on port **18083**, a device mock on
**18765**, and no voice. Phase C restarts production and proves its catalog is
unchanged. Never run the isolated runtime while the production runtime is alive.
Two private `/tmp` directories separate short-lived runtime data from before/after
production evidence; both are removed below after their checks finish.

### Phase A — real production service, non-mutating checks

Create evidence storage and read-only/checking helpers. `m183_file_hash` reads the
production file, with read-only `sudo` fallback; lack of access is recorded, not
worked around by modifying permissions. The API identity comparison is mandatory.

```sh
cd ~/FireflyVoiceAssistantLinux
set -euo pipefail
umask 077
m183_repo=$PWD
m183_evidence=$(mktemp -d /tmp/firefly-m183-evidence-XXXXXX)
m183_prod=http://127.0.0.1:8080
m183_file_hash() {
    if test -r /var/lib/firefly/store/configuration.json; then
        sha256sum /var/lib/firefly/store/configuration.json | awk '{print $1}'
    elif sudo -n test -r /var/lib/firefly/store/configuration.json; then
        sudo -n sha256sum /var/lib/firefly/store/configuration.json | awk '{print $1}'
    else
        return 1
    fi
}
m183_removed() {
    local code
    code=$(curl --max-time 10 -sS -o "$2" -w '%{http_code}' \
      -H 'Content-Type: application/json' -H 'X-Firefly-Admin: 1' \
      -X POST "$1/api/devices/observe" \
      --data '{"target_id":"device_fan","properties":{"power":true}}')
    test "$code" = 404
    python3 - "$2" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
assert r['ok'] is False and r['errors'][0]['code'] == 'not_found', r
print('Removed route: 404 / not_found')
PY
}
m183_ready() {
    python3 - "$1" <<'PY'
import json, sys, time, urllib.request
end = time.monotonic() + 180
while time.monotonic() < end:
    try:
        with urllib.request.urlopen(sys.argv[1] + '/api/status', timeout=2) as response:
            status = json.load(response)
        if status.get('runtime_ready') is True:
            print(json.dumps(status, ensure_ascii=False, indent=2))
            break
    except (OSError, ValueError):
        pass
    time.sleep(1)
else:
    raise SystemExit('Runtime did not become ready; stop and inspect its log')
PY
}
systemctl is-active --quiet firefly-voice-assistant
curl --max-time 10 -fsS "$m183_prod/api/status" > "$m183_evidence/status-before.json"
curl --max-time 10 -fsS -o /dev/null "$m183_prod/"
python3 - "$m183_evidence/status-before.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
assert s['runtime_ready'] is True and s['health']['overall'] == 'ready', s
assert s['voice'] == 'WAITING_WAKE', s
print(json.dumps(s, ensure_ascii=False, indent=2))
PY
m183_removed "$m183_prod" "$m183_evidence/removed-production.json"
curl --max-time 10 -fsS "$m183_prod/api/config" > "$m183_evidence/config-before.json"
if m183_file_hash > "$m183_evidence/sha-before"; then
    cat "$m183_evidence/sha-before"
else
    printf '%s\n' unavailable > "$m183_evidence/sha-before"
    printf '%s\n' 'Production file SHA256 unavailable; API comparison remains required.'
fi
python3 - "$m183_evidence/config-before.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
ids = {'zone_workshop', 'device_fan', 'action_on', 'action_off', 'connection_m183'}
def strings(value):
    if isinstance(value, str): yield value
    elif isinstance(value, dict):
        for item in value.values(): yield from strings(item)
    elif isinstance(value, list):
        for item in value: yield from strings(item)
assert not ids.intersection(strings(r['configuration'])), 'Acceptance ID already present; stop'
print('Production revision:', r['revision'], 'hash:', r['hash'])
PY
```

Record the displayed revision/hash and optional SHA256 in the owner acceptance
report. Open Web Admin through the established access method and verify it loads;
do not edit configuration. Require microphone connected, wake ready, WAITING_WAKE
and overall ready in the status evidence. If an acceptance ID already exists in
configuration or `retired_ids`, stop rather than deleting or restoring anything.

Stop the normal service cleanly and verify there is no competing runtime/NPU owner
before Phase B. If a process remains, stop here and diagnose; do not kill an
unrelated process or launch another model owner.

```sh
sudo systemctl stop firefly-voice-assistant
test "$(systemctl show firefly-voice-assistant -p ActiveState --value)" = inactive
test "$(systemctl show firefly-voice-assistant -p MainPID --value)" = 0
systemctl show firefly-voice-assistant -p Result -p ExecMainStatus
journalctl -u firefly-voice-assistant -n 100 --no-pager \
  | grep -E 'SHUTDOWN complete|RKLLM|Deactivated successfully|Stopped'
if pgrep -af '[f]irefly-demo|[f]irefly-assistant|[f]irefly-admin-runtime'; then
    printf '%s\n' 'A runtime remains; stop before isolated acceptance.'
    exit 1
fi
```

Require clean shutdown evidence (`SHUTDOWN complete`, RKLLM destroyed, systemd
successful stop), `Result=success` and `ExecMainStatus=0`.

### Phase B — isolated state-flow acceptance

Create a fresh temporary ConfigStore and init configuration entirely from repository
fixtures, never from the production store. Select only the workshop, fan and its two
power actions; all HTTP bindings/observations point at the loopback mock. The normal
model/runtime remain read-only. `--voice` is deliberately omitted; this test needs
no microphone or wake worker. Optional Admin STT may still initialize normally.

```sh
m183_dir=$(mktemp -d /tmp/firefly-m183-runtime-XXXXXX)
m183_base=http://127.0.0.1:18083
mkdir -m 700 "$m183_dir/store"
printf '%s\n' "$m183_repo" > "$m183_dir/repo.txt"
printf 'Record isolated directory for cross-shell cleanup: %s\n' "$m183_dir"
python3 - "$m183_repo" "$m183_dir" <<'PY'
import json, pathlib, socket, sys
repo, p = map(pathlib.Path, sys.argv[1:])
# Reject occupied ports; never stop an unrelated listener.
for port in (18083, 18765):
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', port))
c = json.loads((repo / 'config/empty-home.json').read_text())
f = json.loads((repo / 'tests/admin/v2/system.json').read_text())
c['rooms'] = [r for r in f['rooms'] if r['id'] == 'zone_workshop']
c['devices'] = [d for d in f['devices'] if d['id'] == 'device_fan']
c['actions'] = [a for a in f['actions'] if a['id'] in ('action_on', 'action_off')]
assert len(c['rooms']) == len(c['devices']) == 1 and len(c['actions']) == 2
c['connections'] = [{'id': 'connection_m183', 'name': 'M18.3 isolated mock',
    'type': 'http', 'enabled': True, 'timeout_ms': 1000,
    'http': {'base_url': 'http://127.0.0.1:18765', 'base_path': ''}}]
c['bindings'] = [{'target_id': 'device_fan', 'action_id': 'action_' + action,
    'connection_id': 'connection_m183', 'executor': 'http',
    'http': {'method': 'POST', 'path': '/relay/1', 'body': {'on': action == 'on'},
             'expected_status': 200}} for action in ('on', 'off')]
c['observations'] = [{'target_id': 'device_fan', 'connection_id': 'connection_m183',
    'executor': 'http', 'http': {'method': 'GET', 'path': '/relay/1', 'expected_status': 200},
    'fields': {'on': 'power'}}]
(p / 'init.json').write_text(json.dumps(c, ensure_ascii=False, indent=2))
PY
```

Create this temporary cleanup helper **before launching either process**. It reads
saved PIDs, checks the process UID and exact isolated arguments in
`/proc/<pid>/cmdline` before SIGTERM, and polls `os.kill(pid, 0)` (the same existence
check as `kill -0`) for up to 60 seconds using monotonic time. It checks the relevant
port and scans `/proc` for matching processes afterwards. A timeout, identity
mismatch, permission error, remaining process or occupied port stops cleanup and
preserves the directory. An already-gone PID is acceptable only after the same
port/process checks. No `wait PID` is used; a shell may use it additionally only
for a child it actually owns. Cross-shell cleanup cannot report a child exit code.

```sh
cat > "$m183_dir/stop-isolated.py" <<'PY'
import os, pathlib, signal, socket, sys, time
p = pathlib.Path(__file__).resolve().parent
kind = sys.argv[1]
assert kind in ('mock', 'demo'), kind
pid = int((p / (kind + '.pid')).read_text())
assert pid > 1
repo = pathlib.Path((p / 'repo.txt').read_text().strip())
port = 18765 if kind == 'mock' else 18083

def argv(number):
    try:
        return (pathlib.Path('/proc') / str(number) / 'cmdline').read_bytes().split(b'\0')[:-1]
    except FileNotFoundError:
        return None

def matches(args):
    if not args:
        return False
    if kind == 'mock':
        return (os.fsencode(p / 'mock-runner.py') in args and
                os.fsencode(repo / 'scripts/mock-http-device.py') in args and
                os.fsencode(p / 'device-requests.jsonl') in args)
    return (args[0] == os.fsencode(repo / 'build/firefly-demo') and
            any(args[i:i+2] == [b'--store', os.fsencode(p / 'store')]
                for i in range(len(args) - 1)))

def exists():
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

if exists():
    args = argv(pid)
    assert pathlib.Path('/proc', str(pid)).stat().st_uid == os.getuid(), 'PID owner mismatch'
    assert matches(args), f'PID {pid} identity mismatch; do not signal or delete: {args!r}'
    print('Verified /proc identity:', pid, args, flush=True)
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + 60
    while exists():
        if time.monotonic() >= deadline:
            raise SystemExit('Process still exists; preserve files, inspect, do not restart production')
        time.sleep(0.2)
assert not exists(), 'Process has not disappeared'
# SO_REUSEADDR tolerates residual TCP TIME_WAIT, not an active listener.
with socket.socket() as probe:
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    probe.bind(('127.0.0.1', port))
for entry in pathlib.Path('/proc').iterdir():
    if not entry.name.isdigit():
        continue
    try:
        args = argv(int(entry.name))
    except PermissionError:
        raise SystemExit('Cannot inspect process identities; stop and verify before cleanup')
    assert not matches(args), f'Matching isolated process remains: {entry.name}'
print(f'{kind}: PID gone, port {port} free, no matching isolated process remains')
PY
```

The standalone mock CLI suppresses request logging. This temporary launcher uses
**the existing `scripts/mock-http-device.py` server and handler unchanged**, exposing
its in-process request list as a flushed JSONL file and providing clean SIGTERM
shutdown. It adds no Firefly API or state-injection seam. Only explicit device
requests below may appear in the log; do not open a device URL in a browser.

```sh
cat > "$m183_dir/mock-runner.py" <<'PY'
import importlib.util, json, signal, sys, threading
spec = importlib.util.spec_from_file_location('m183_mock', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
stop = threading.Event()
signal.signal(signal.SIGTERM, lambda *_: stop.set())
signal.signal(signal.SIGINT, lambda *_: stop.set())
with open(sys.argv[2], 'w', buffering=1) as log:
    class LoggedRequests(list):
        def append(self, item):
            super().append(item)
            print(json.dumps(item), file=log, flush=True)
    server = module.MockDevice(('127.0.0.1', 18765))
    server.requests = LoggedRequests()
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    print('MOCK_LISTEN http://127.0.0.1:18765/relay/1', flush=True)
    try:
        stop.wait()
    finally:
        server.shutdown()
        thread.join()
        server.server_close()
PY
python3 -u "$m183_dir/mock-runner.py" "$m183_repo/scripts/mock-http-device.py" \
  "$m183_dir/device-requests.jsonl" > "$m183_dir/mock.log" 2>&1 &
m183_mock_pid=$!
printf '%s\n' "$m183_mock_pid" > "$m183_dir/mock.pid"
for m183_i in $(seq 1 50); do
    kill -0 "$m183_mock_pid"
    if grep -q '^MOCK_LISTEN ' "$m183_dir/mock.log"; then break; fi
    sleep 0.1
done
grep '^MOCK_LISTEN ' "$m183_dir/mock.log"
LD_LIBRARY_PATH=/home/firefly/demo_Linux_aarch64/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  "$m183_repo/build/firefly-demo" \
  --store "$m183_dir/store" --init-config "$m183_dir/init.json" \
  --model /home/firefly/demo_Linux_aarch64/Qwen3-4B_W8A8_RK3588_3core.rkllm \
  --admin-bind 127.0.0.1 --admin-port 18083 \
  > "$m183_dir/demo.log" 2>&1 &
m183_demo_pid=$!
printf '%s\n' "$m183_demo_pid" > "$m183_dir/demo.pid"
printf 'Isolated demo PID=%s; mock PID=%s; directory=%s\n' \
  "$m183_demo_pid" "$m183_mock_pid" "$m183_dir"
m183_ready "$m183_base"
kill -0 "$m183_demo_pid"
m183_removed "$m183_base" "$m183_dir/removed-isolated.json"
curl --max-time 10 -fsS "$m183_base/api/config/active" > "$m183_dir/active.json"
python3 - "$m183_dir" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
active = json.loads((p / 'active.json').read_text())['active']
assert active and active['hash']
(p / 'command.json').write_text(json.dumps({'text': 'включи вентилятор в цехе',
    'expected_hash': active['hash'], 'expected_action': {
        'target_id': 'device_fan', 'action_id': 'action_on', 'arguments': {}}}, ensure_ascii=False))
(p / 'poll.json').write_text(json.dumps({'target_id': 'device_fan', 'expected_hash': active['hash']}))
PY
m183_state() {
    curl --max-time 10 -fsS "$m183_base/api/devices/state" > "$m183_dir/state.json"
    python3 - "$m183_dir/state.json" "$1" "$2" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
d = next(d for d in r['devices'] if d['targetId'] == 'device_fan')
p = d['properties']['power']
assert p['knowledge'] == sys.argv[2] and p['value'] is json.loads(sys.argv[3]), d
print(json.dumps(d, ensure_ascii=False, indent=2))
PY
}
m183_requests() {
    python3 - "$m183_dir/device-requests.jsonl" "$1" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert [r[0] for r in rows] == sys.argv[2].split(), rows
assert all(r[1] == '/relay/1' for r in rows), rows
print('Device requests:', rows)
PY
}
m183_post() {
    curl --max-time 10 -fsS -H 'Content-Type: application/json' -H 'X-Firefly-Admin: 1' \
      --data-binary @"$m183_dir/$2.json" "$m183_base/api/devices/$1"
}
m183_state unknown null
m183_requests ''
```

This proves removed-route 404 and initial **Unknown** with zero hardware requests,
even though the configuration contains a logical initial value. Next execute the
guarded command and read state repeatedly. Require command success, **Predicted true**
and exactly one device POST with **zero GETs** after all state reads.

```sh
m183_post command command > "$m183_dir/command-result.json"
python3 - "$m183_dir/command-result.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
assert r['execution']['success'] is True, r
print(json.dumps(r, ensure_ascii=False, indent=2))
PY
m183_state predicted true
m183_state predicted true
m183_state predicted true
m183_requests 'POST'
m183_post poll poll | python3 -m json.tool
m183_state confirmed true
m183_requests 'POST GET'
```

The explicit poll must add one actual device GET and change knowledge to
**Confirmed true**. Change the mock independently through its **device URL**:

```sh
curl --max-time 10 -fsS -H 'Content-Type: application/json' --data '{"on":false}' \
  http://127.0.0.1:18765/relay/1 | python3 -m json.tool
m183_state confirmed true
m183_requests 'POST GET POST'
m183_post poll poll | python3 -m json.tool
m183_state confirmed false
m183_requests 'POST GET POST GET'
cp "$m183_dir/state.json" "$m183_dir/confirmed-before-failure.json"
```

Before the second poll, cached Confirmed remains true; after its GET, Confirmed
becomes false. Now stop only the owned mock cleanly and poll an unreachable device.
Require HTTP 422 / `connection_failure`, unreachable connectivity, unchanged false
value, knowledge and evidence timestamps. A prior Confirmed value can remain cached;
staleness may advance with time and is not new observation evidence.

```sh
python3 "$m183_dir/stop-isolated.py" mock
m183_code=$(curl --max-time 10 -sS -o "$m183_dir/failed-poll.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -H 'X-Firefly-Admin: 1' \
  --data-binary @"$m183_dir/poll.json" "$m183_base/api/devices/poll")
test "$m183_code" = 422
m183_state confirmed false
python3 - "$m183_dir" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
r = json.loads((p / 'failed-poll.json').read_text())
assert r['ok'] is False and r['errors'][0]['code'] == 'connection_failure', r
def device(name):
    return next(d for d in json.loads((p / name).read_text())['devices'] if d['targetId'] == 'device_fan')
before, after = device('confirmed-before-failure.json'), device('state.json')
assert after['connectivity'] == 'unreachable', after
for key in ('knowledge', 'value', 'observed_at_ms', 'updated_at_ms'):
    assert before['properties']['power'][key] == after['properties']['power'][key], key
print('Failed poll preserved existing evidence:', r)
PY
m183_requests 'POST GET POST GET'
python3 "$m183_dir/stop-isolated.py" demo
grep -F 'SHUTDOWN complete' "$m183_dir/demo.log"
grep -F 'RKLLM destroyed' "$m183_dir/demo.log"
```

Require **actual process disappearance**, free relevant ports, no matching processes
and both shutdown-complete and RKLLM-destroyed log evidence. SIGTERM delivery or
`SHUTDOWN complete` alone does not mean the listener or model teardown has finished.
Record state/results/request logs in the owner report or copy evidence to the
separate evidence directory before deleting the runtime directory.

For cleanup from **another shell**, first recover the exact directory printed at
launch (do not select a directory by wildcard or assume the most recent one).
The helper and PID files require no functions or child ownership from the original
shell. This optional block sets the directory only; do not rerun launch commands:

```sh
read -r -p 'Exact existing /tmp/firefly-m183-runtime-... directory: ' m183_dir
case "$m183_dir" in /tmp/firefly-m183-runtime-*) ;; *) exit 1 ;; esac
test -f "$m183_dir/stop-isolated.py"
test -f "$m183_dir/mock.pid"
test -f "$m183_dir/demo.pid"
```

Run the following final verification in either shell. Rechecking already-stopped
PIDs is intentional. If interrupted earlier, it gracefully stops only the identified
acceptance processes. Preserve evidence outside this directory before the last line.
No deletion occurs unless **every** check succeeds:

```sh
(
    set -euo pipefail
    case "$m183_dir" in /tmp/firefly-m183-runtime-*) ;; *) exit 1 ;; esac
    python3 "$m183_dir/stop-isolated.py" mock
    python3 "$m183_dir/stop-isolated.py" demo
    grep -F 'SHUTDOWN complete' "$m183_dir/demo.log"
    grep -F 'RKLLM destroyed' "$m183_dir/demo.log"
    test "$(systemctl show firefly-voice-assistant -p ActiveState --value)" = inactive
    test "$(systemctl show firefly-voice-assistant -p MainPID --value)" = 0
    if pgrep -af '[f]irefly-demo|[f]irefly-assistant|[f]irefly-admin-runtime|[m]ock-runner.py|[m]ock-http-device.py'; then
        printf '%s\n' 'A Firefly/mock process remains; preserve files and diagnose.'
        exit 1
    fi
    python3 - <<'PY'
import socket
for port in (8080, 18083, 18765):
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', port))
    print('Port free:', port)
PY
    rm -r -- "$m183_dir"
)
```

If any check fails, preserve the directory and logs, leave production stopped and
diagnose. A zombie still visible to `kill -0` has not passed this procedure; its
owning parent must reap it before cleanup can pass. Never signal an unrelated or
reused PID on an identity mismatch. Do not launch production while an isolated
runtime remains. The corrected cleanup was syntax-checked during documentation
finalization; the real follow-up process/port verification is recorded above.

### Phase C — return to production and prove the catalog unchanged

Restart the existing unit without any unit or configuration changes. Compare both
API revision/hash **and the complete configuration including `retired_ids`** to
Phase A. If the file SHA256 was available before, it must also be available and
identical now. No restore or catalog cleanup is needed or permitted.

```sh
sudo systemctl start firefly-voice-assistant
systemctl is-active --quiet firefly-voice-assistant
m183_ready "$m183_prod"
curl --max-time 10 -fsS -o /dev/null "$m183_prod/"
curl --max-time 10 -fsS "$m183_prod/api/config" > "$m183_evidence/config-after.json"
python3 - "$m183_evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
before = json.loads((p / 'config-before.json').read_text())
after = json.loads((p / 'config-after.json').read_text())
for key in ('revision', 'hash', 'configuration'):
    assert before[key] == after[key], f'Production {key} changed; stop and report, do not restore'
ids = {'zone_workshop', 'device_fan', 'action_on', 'action_off', 'connection_m183'}
def strings(value):
    if isinstance(value, str): yield value
    elif isinstance(value, dict):
        for item in value.values(): yield from strings(item)
    elif isinstance(value, list):
        for item in value: yield from strings(item)
assert not ids.intersection(strings(after['configuration'])), 'Acceptance IDs found in production'
print('Unchanged production revision:', after['revision'], 'hash:', after['hash'])
print('Configuration and retired_ids unchanged; no acceptance IDs present.')
PY
if test "$(cat "$m183_evidence/sha-before")" != unavailable; then
    m183_file_hash > "$m183_evidence/sha-after"
    cmp "$m183_evidence/sha-before" "$m183_evidence/sha-after"
    cat "$m183_evidence/sha-after"
fi
curl --max-time 10 -fsS "$m183_prod/api/status" | python3 -m json.tool
journalctl -u firefly-voice-assistant -n 100 --no-pager \
  | grep -E 'VOICE_STATE|VOICE text=|VOICE_RECOVERY|HEALTH'
```

Wait for microphone connected, wake ready, voice WAITING_WAKE and health overall
ready; runtime readiness alone can precede voice startup. Open Web Admin through
the established access method and verify it remains healthy without saving anything.
Say «Амина», then «Какая погода?»; require wake detection, beep, listening/STT,
safe Unsupported on the production empty domain, and return to WAITING_WAKE.
Record that voice evidence and final healthy status:

```sh
systemctl is-active --quiet firefly-voice-assistant
curl --max-time 10 -fsS "$m183_prod/api/status" | python3 -m json.tool
journalctl -u firefly-voice-assistant -n 150 --no-pager \
  | grep -E 'WAKE_DETECTED|BEEP_START|BEEP_END|LISTENING_COMMAND|SPEECH_START|SPEECH_END|VOICE|Unsupported|target_not_grounded'
# Only after recording the before/after comparisons and owner evidence:
case "$m183_evidence" in /tmp/firefly-m183-evidence-*) ;; *) exit 1 ;; esac
rm -r -- "$m183_evidence"
unset m183_evidence
```

Owner acceptance is **COMPLETE / PASS**, as recorded above. For any repeat run, a
failed identity/hash/retired-ID check is a blocker: report it, never attempt a restore
as cleanup. The production catalog must remain unchanged by acceptance.

## Known limitations and boundaries

Configured GET evidence is trusted device telemetry, not cryptographic proof of
physical state. A dishonest device or an intentionally configured mock can report
false data; this milestone removes direct public state injection, not that trust
boundary. Trusted in-process/private IPC code can still use the internal seam.
Existing origin/content-type/Admin-header guards, loopback deployment, stale
evidence and error semantics are unchanged. TLS/authentication, Host/LAN policy,
new transports, persistence and release operations remain out of scope. Owner
acceptance is COMPLETE / PASS; automated tests use only isolated fake HTTP devices/stores.
This finalization changes documentation only. The validated focused 14/14 and full
CTest 74/74 (including Chromium) results stand; no implementation/test change
required rerunning those suites.
