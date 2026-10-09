# M18.4 — Web Admin Network Security Hardening

Baseline: `fcffe2fc7b0965fd77ff631c1f566072ee3f06f5`, branch
`hardening/admin-network-security`. M18.1–M18.3 remain COMPLETE / PASS.
M18.4 automated verification and real **owner acceptance are COMPLETE / PASS**.

## Threat model and previous exposure

The unauthenticated Admin listener defaulted to loopback, and the production unit
already specified `--admin-bind 127.0.0.1`. However, both entrypoints accepted other
explicit IPv4 addresses, including wildcard/LAN bindings, after store initialization
and possible worker startup. A mistaken argument could expose Admin directly to LAN.

HttpServer required a Host header but did not validate its authority. Its mutation
Origin guard compared `Origin` with `http://` plus that untrusted Host. A hostile
site whose DNS rebinds to loopback could send matching `Host: attacker.example`
and `Origin: http://attacker.example`. Matching those two attacker-controlled values
is not a trusted-origin boundary. GETs also needed protection against reading
configuration, status, device state and static assets through a rebound hostname.

## V1 policy and implementation

Web Admin is **loopback-only**. Both `firefly-demo` and `firefly-admin` accept exactly
`--admin-bind 127.0.0.1` (also the default). Wildcard, LAN IP, `127.0.0.2`, `::1` and
`localhost` listener arguments are rejected. There is no LAN opt-in, environment
bypass, magic header, proxy trust mode or CORS. The shared `validateAdminBind` runs
immediately after argument parsing, before ConfigStore/seed initialization, worker
spawn, model ownership, STT/voice initialization or listener creation. HttpServer
also enforces the same policy at its constructor boundary.

`include/firefly/admin/NetworkPolicy.h` and `src/admin/NetworkPolicy.cpp` contain
the policy. Main/DemoMain call the shared validator; HttpServer uses the strict
ASCII hostname/port parser before routing **every** request. Api, ConfigStore,
DeviceStateStore, guarded commands, SafetyGate, voice and recovery are unchanged.
The systemd unit and helper retain their existing `127.0.0.1` arguments unchanged.

| Input | Contract |
| --- | --- |
| Host `127.0.0.1` or case-insensitive `localhost` | Accepted |
| Either hostname with optional decimal port 1–65535 | Accepted; port need not equal the backend listener port |
| Valid foreign hostname/IPv4 authority, including lookalikes and trailing-dot aliases | HTTP **403**, `errors[0].code=http_request`, `errors[0].message="Host is not allowed."` |
| Missing/empty/duplicate Host or malformed authority | HTTP **400**, `errors[0].code=http_request` |
| Invalid authority syntax reaching the authority parser | Message `Malformed Host authority.`; earlier framing/header checks retain their existing generic messages |
| Mutation Origin absent | Existing curl/local API use remains supported |
| Mutation Origin present | Must use `http://`, an allowed hostname, and the same normalized hostname and numeric port as Host; omitted port means 80 |
| Invalid, foreign, `null`, HTTPS or mismatched mutation Origin | HTTP **403**, `Cross-origin mutation rejected.` |

The authority parser accepts ASCII DNS-style labels and an optional decimal port;
it never resolves names or uses permissive URL parsing. Userinfo, lists, embedded
whitespace/control characters, paths, queries, fragments, percent encoding, IPv6,
empty/nondecimal/out-of-range ports and trailing data are invalid. Normal HTTP
header-edge optional whitespace is handled by the existing header parser; it does
not normalize embedded whitespace or URL syntax into a trusted hostname. Only the
two exact allowed names after ASCII case folding are trusted. `localhost` and
`127.0.0.1` are distinct origins, not interchangeable aliases for Origin comparison.
Numeric default-port and hostname-case normalization do not weaken that boundary.
No error reflects the supplied hostile Host value.

Existing Admin-header, cross-site fetch, content-type, duplicate-header, size,
Content-Length, Transfer-Encoding and Expect guards remain in force. Host rejection
happens before API dispatch/body interpretation, including before a mutation whose
Origin matches the foreign Host. `/api/devices/observe` remains 404 under an allowed
Host; a forbidden Host gets 403 before route lookup.

## Remote access and compatibility

The supported owner workflow is an authenticated **SSH TCP tunnel**, including SSH
over a board's Tailscale address. Tailscale itself does not authorize a direct HTTP
Admin listener. From the operator's laptop:

```sh
ssh -N -L 18080:127.0.0.1:8080 firefly@BOARD
```

Open `http://127.0.0.1:18080/` or `http://localhost:18080/`. The forwarded Host port
18080 is deliberately accepted even when Firefly listens on 8080. The browser's
matching HTTP Origin remains valid. No Tailscale API integration is involved.

Explicit LAN/wildcard launches and arbitrary DNS aliases cease to work. Listener
IPv6 and hostname binds are not supported. Existing Chromium/manual API fixtures
use loopback and require no migration. Historical M9/M10/M11 LAN evidence is marked
as historical, with current launch/help guidance pointing to loopback/tunnels.
Device HTTP executor connections to LAN equipment are a separate outbound path and
remain unchanged. No frontend source or bundle changes are needed.

## Automated verification

`AdminNetworkSecurity` exercises real Api/HttpServer and Main, with private stores
and a fake inference worker. It checks:

- both actual production CLIs reject all six forbidden bind examples before creating
  **any** fresh store file or launching a marker worker; no demo startup/model path;
- default and explicit loopback start correctly (actual Main and actual DemoMain
  linked to fake RKLLM for successful startup, without voice/NPU use);
- static/API GET, case-insensitive localhost, port bounds and tunnel-style ports;
- valid mutation Origin and absent-Origin local API behavior;
- foreign Host denial on `/`, status, config and device state, plus a valid Apply
  POST with a matching hostile Origin and Admin/content-type headers;
- no store/file/config/device-state changes after forbidden requests;
- lookalikes, malformed authorities, missing/duplicate Host and mutation guards;
- M18.3's removed observation route remains 404 with valid local headers.

Commands use existing configured offline dependencies and isolated test fixtures:

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j2
ctest --test-dir build -R 'AdminNetworkSecurity|AdminHttp|AdminManualApi|AdminGuardedCommand|AdminSetup|ProductionApiHardening|ProductionStartup|VoiceRecovery' --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

Results (2026-10-09):

- Configure/build PASS; no compiler warnings in the build log.
- New AdminNetworkSecurity: **6 cases PASS** (standalone CTest **9.37 s**).
- Focused **11/11 PASS, 97.51 s**, including AdminHttp, guarded/manual/setup APIs,
  ProductionApiHardening, ProductionStartupContract, ProductionStartup,
  VoiceRecovery/Integration and AdminSetupV2Chromium.
- Full CTest: **75/75 PASS, 286.94 s**, including AdminV2Chromium (**62.31 s**),
  AdminManualChromium (**46.29 s**) and AdminSetupV2Chromium (**42.30 s**).
- `git diff --check` PASS; new untracked files separately checked for whitespace errors.
- Owner runbook, including the controlled-restart correction: shell and embedded
  Python blocks passed syntax checks without executing owner commands.

Logs: `/tmp/firefly-m184-configure.log`, `/tmp/firefly-m184-build.log`,
`/tmp/firefly-m184-focused.log`, `/tmp/firefly-m184-ctest.log`.
No owner acceptance, installed service operation, production-store mutation or
NPU/physical-voice acceptance is part of these tests.

## Final real owner acceptance — COMPLETE / PASS

The owner completed the following real appliance checks. This is owner-supplied
acceptance evidence, separate from the automated results above. Documentation
finalization did not execute acceptance or change the validated implementation.

### 1. Controlled M18.4 production load — PASS

Before restart, the service was healthy: **MainPID=196756**, `runtime_ready=true`,
`voice=WAITING_WAKE`, `microphone=connected`, overall ready and wake ready.
Production configuration was revision **1**, `retired_ids=[]`, with both API hash
and `configuration.json` SHA256:

`36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`

The approved M18.4 disk binary SHA256 was:

`21ee9c9c7d8d1971e6cd9329d6371ed62b5a8da32d6dd9fe6c2a22cfd5bbafb7`

The running old executable appeared as **`firefly-demo (deleted)`**, with SHA256:

`a3f46144b1dfb7528810398e6534031f6b4bfdc7023135389759ce210844ae75`

This proved the running service retained the previous executable image despite
the M18.4 build on disk. Exactly one controlled
`sudo systemctl restart firefly-voice-assistant` loaded the approved build using
the existing installed unit; no unit reinstall/edit was needed.

After restart: **MainPID=212461**, **NRestarts=0**, `ActiveState=active`,
`Result=success`. Full readiness returned: `runtime_ready=true`,
`voice=WAITING_WAKE`, `microphone=connected`, overall ready and wake ready.
Both the running process image and disk binary had exactly the same SHA256:

`21ee9c9c7d8d1971e6cd9329d6371ed62b5a8da32d6dd9fe6c2a22cfd5bbafb7`

The journal showed clean old-service stopping, `SHUTDOWN complete`,
`RKLLM destroyed`, old service stopped, new `STARTUP` process, Admin HTTP
listening, runtime ready, `VOICE_STATE WAITING_WAKE` and `HEALTH -> ready`.

Immediately after loading M18.4, production revision remained **1**, the API hash
and file SHA256 remained
`36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`,
`retired_ids=[]`, and the complete configuration was unchanged.

### 2. Production listener / Host policy — PASS

The only Admin listener was **127.0.0.1:8080**, with no wildcard or LAN listener.
Normal local `GET /` returned **HTTP 200**. Tunnel-style Host values
`127.0.0.1:18080` and `LOCALHOST:18080` both returned **HTTP 200**, proving the
Host port is intentionally independent of the backend listener port.

`Host: attacker.example` against `/`, `/api/status` and `/api/config` returned
**HTTP 403**, with the generic message **`Host is not allowed.`**

| Owner-tested authority | Result |
| --- | --- |
| `localhost.evil` | HTTP 403 |
| `127.0.0.1.evil` | HTTP 403 |
| `0.0.0.0` | HTTP 403 |
| `192.168.68.111` | HTTP 403 |
| `localhost:abc` | HTTP 400 |
| `localhost:99999` | HTTP 400 |
| `127.0.0.1:abc` | HTTP 400 |
| `localhost@evil` | HTTP 400 |
| `user@127.0.0.1` | HTTP 400 |

Production remained active with **MainPID=212461**, **NRestarts=0**, and
runtime/voice/microphone/wake ready.

### 3. Isolated bind policy — PASS

Only temporary isolated stores were used. All four cases exited **1** before
creating `configuration.json`:

| Entrypoint | Rejected `--admin-bind` |
| --- | --- |
| `firefly-admin` | `0.0.0.0` |
| `firefly-admin` | `192.168.68.111` |
| `firefly-demo` | `0.0.0.0` |
| `firefly-demo` | `192.168.68.111` |

The startup error explicitly stated:

> V1 Admin bind must be exactly 127.0.0.1; use an SSH/Tailscale TCP tunnel for remote access.

This proved arbitrary LAN/wildcard binds are rejected before ConfigStore
initialization, for both production entrypoints.

### 4. DNS rebinding regression — PASS

An isolated loopback `firefly-admin` accepted `Host: localhost:18080` with
**HTTP 200**. The owner then sent **`POST /api/config/reconcile`** with:

```text
Host: attacker.example
Origin: http://attacker.example
X-Firefly-Admin: 1
Content-Type: application/json
```

It returned **HTTP 403**, error code **`http_request`**, and message
**`Host is not allowed.`** The temporary ConfigStore SHA256 before and after
was exactly:

`36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`

Matching attacker-controlled Host/Origin did not bypass policy; rejection occurred
before API mutation and the store SHA remained unchanged. The isolated Admin then
stopped cleanly. This records the actual owner-tested reconcile request; the
repeatable runbook below retains its isolated, non-persisting validation request.

### 5. Real SSH/tunnel workflow — PASS

From the owner's laptop, the actual tunnel command was:

```sh
ssh -o ExitOnForwardFailure=yes \
    -N \
    -L 18080:127.0.0.1:8080 \
    firefly@firefly
```

The real browser/API path through the tunnel worked:

- `http://127.0.0.1:18080/` returned **HTTP 200**.
- `/api/status` through `127.0.0.1:18080` reported `runtime_ready=true`,
  `voice=WAITING_WAKE`, `microphone=connected`, overall ready and wake ready.
- `/api/status` through `localhost:18080` reported `runtime_ready=true`,
  `voice=WAITING_WAKE`, `microphone=connected` and overall ready.
- The tunnel stopped normally and laptop port **18080** was released.

This verifies the supported V1 SSH/Tailscale TCP-tunnel workflow through a real
SSH tunnel, without direct LAN HTTP Admin or Tailscale API integration.

### 6. Final production integrity — PASS

After all acceptance checks, production still matched the **original pre-restart
baseline**, not merely a post-restart snapshot: revision **1**, `retired_ids=[]`,
and the complete configuration unchanged. The API hash and final
`configuration.json` SHA256 were both:

`36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be`

This exactly matched the original pre-M18.4 SHA. Final systemd state was
**MainPID=212461**, **NRestarts=0**, `ActiveState=active`, `Result=success`.
Health remained `runtime_ready=true`, `voice=WAITING_WAKE`,
`microphone=connected`, overall ready and wake ready. Neither loading M18.4 nor
subsequent acceptance mutated the production ConfigStore.

### 7. Final real voice smoke — PASS

The production M18.4 voice cycle produced `WAKE_DETECTED` → `BEEP_START` →
`BEEP_END` → `LISTENING_COMMAND` → `SPEECH_START` → `SPEECH_END` → `PROCESSING`.
The STT transcript was exactly **«какая погода»**. The empty production domain
safely returned `unsupported=true` / `target_not_grounded`, then returned to
**WAITING_WAKE**.

Final status: **MainPID=212461**, **NRestarts=0**, `runtime_ready=true`,
`voice=WAITING_WAKE`, `microphone=connected`,
`last_voice.transcript="какая погода"`, overall ready and wake ready.

### Final M18.4 conclusion

**Owner acceptance COMPLETE / PASS.** V1 Web Admin is loopback-only. Both CLI
entrypoints reject every bind value other than exactly `127.0.0.1`, before
ConfigStore initialization. Every HTTP request validates Host before routing;
accepted authorities are `localhost` (case-insensitive) and `127.0.0.1`, with an
optional valid decimal port. Foreign Host is rejected with **403**, malformed
Host authority with **400**.

Matching foreign Host + Origin does not bypass policy, and rejected foreign
requests do not mutate ConfigStore. Tunnel-style ports and the real SSH/Tailscale
TCP-tunnel workflow are supported and verified; direct LAN HTTP Admin is not
supported in V1. Production configuration remained unchanged throughout acceptance,
and voice/runtime/Web Admin remained healthy after the controlled load. No TLS,
authentication, CORS or reverse-proxy mode was added. Existing focused **11/11**
and full CTest **75/75** PASS results remain applicable; this documentation-only
finalization does not rerun those suites.

## Owner acceptance runbook — repeatable procedure, production configuration unchanged

The approved M18.4 `build/firefly-demo` must already be built. Replacing that file
on disk does **not** replace an already-running M18.3 executable image. This runbook
therefore requires **exactly one controlled production restart** to load M18.4,
using the existing installed unit and current build. Do not reinstall, edit or
reload the unit; its `--admin-bind 127.0.0.1` argument is already correct.

These commands are prepared for the owner, **not executed by the coding agent**.
Do not Save, Apply, import, restore or edit production configuration or
`/var/lib/firefly/store`. Apart from the single explicit restart below, no production
service operation is required. XVF3800 firmware >=2.0.10 remains a prerequisite
(2.0.10 directly validated). Stop on any failure and retain evidence; do not retry
the restart or restore/repair configuration to make acceptance pass.

### A0. Original pre-restart production evidence

Use one board Bash session. Create a private evidence directory and retain it until
all comparisons are recorded. These `*-before` files are the **original pre-restart
baseline**, used both immediately after loading M18.4 and again in Phase G. Never
replace them with a post-restart baseline. File SHA256 uses read-only sudo; if access
is unavailable, stop and arrange read access without changing store permissions.

```sh
cd ~/FireflyVoiceAssistantLinux
set -euo pipefail
umask 077
m184_evidence=$(mktemp -d /tmp/firefly-m184-evidence-XXXXXX)
m184_base=http://127.0.0.1:8080
systemctl is-active --quiet firefly-voice-assistant
systemctl show firefly-voice-assistant -p ActiveState -p MainPID -p NRestarts -p Result \
  | tee "$m184_evidence/service-before.txt"
m184_old_pid=$(systemctl show firefly-voice-assistant -p MainPID --value)
test "$m184_old_pid" -gt 0
printf '%s\n' "$m184_old_pid" > "$m184_evidence/pid-before"
curl --max-time 10 -fsS "$m184_base/api/status" > "$m184_evidence/status-before.json"
curl --max-time 10 -fsS "$m184_base/api/config" > "$m184_evidence/config-before.json"
sudo sha256sum /var/lib/firefly/store/configuration.json \
  | tee "$m184_evidence/sha-before"
python3 - "$m184_evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
s = json.loads((p / 'status-before.json').read_text())
assert (s['runtime_ready'] is True and s['voice'] == 'WAITING_WAKE' and
        s['microphone'] == 'connected' and s['health']['overall'] == 'ready' and
        s['health']['subsystems']['wake']['status'] == 'ready'), s
print(json.dumps(s, ensure_ascii=False, indent=2))
c = json.loads((p / 'config-before.json').read_text())
print('Original pre-restart revision/hash:', c['revision'], c['hash'])
print('Original retired_ids:', c['configuration']['retired_ids'])
PY
```

This records the active service, old MainPID, NRestarts, full healthy status,
complete `/api/config` response, revision/hash and production file SHA256 before
any restart or M18.4 network-policy assertions.

### A1. Load the approved M18.4 binary exactly once

Confirm the approved executable exists and record its digest. Record a journal
cursor before restarting so the subsequent evidence includes both the old process's
graceful shutdown and the new process's startup. Do not rebuild during acceptance.

```sh
test -f "$PWD/build/firefly-demo"
test -x "$PWD/build/firefly-demo"
sha256sum "$PWD/build/firefly-demo" | awk '{print $1}' \
  | tee "$m184_evidence/approved-binary.sha256"
m184_journal_cursor=$(sudo journalctl -u firefly-voice-assistant -n 1 \
  --show-cursor --no-pager | sed -n 's/^-- cursor: //p')
test -n "$m184_journal_cursor"
printf '%s\n' "$m184_journal_cursor" > "$m184_evidence/journal-cursor"
# The only production restart in this acceptance procedure:
sudo systemctl restart firefly-voice-assistant
systemctl is-active --quiet firefly-voice-assistant
m184_new_pid=$(systemctl show firefly-voice-assistant -p MainPID --value)
test "$m184_new_pid" -gt 0
test "$m184_new_pid" != "$m184_old_pid"
printf '%s\n' "$m184_new_pid" > "$m184_evidence/pid-loaded"
```

An active unit alone does not prove full readiness. Wait up to 180 seconds for
runtime, voice, microphone, overall health **and wake** to become ready, then require
10 seconds of stable full readiness with the same new MainPID. A PID change or
failed readiness check stops acceptance. Record NRestarts as evidence rather than
requiring a particular value across the explicit manual restart.

```sh
python3 - "$m184_base" "$m184_new_pid" "$m184_evidence" <<'PY'
import json, pathlib, subprocess, sys, time, urllib.request
base, expected_pid, directory = sys.argv[1:]
p = pathlib.Path(directory)
def service():
    text = subprocess.check_output(['systemctl', 'show', 'firefly-voice-assistant',
        '-p', 'MainPID', '-p', 'ActiveState', '-p', 'NRestarts', '-p', 'Result'], text=True)
    values = dict(line.split('=', 1) for line in text.splitlines())
    assert values['MainPID'] == expected_pid and values['ActiveState'] == 'active', values
    return text
def status():
    with urllib.request.urlopen(base + '/api/status', timeout=2) as response:
        return json.load(response)
def ready(s):
    return (s.get('runtime_ready') is True and s.get('voice') == 'WAITING_WAKE' and
            s.get('microphone') == 'connected' and s.get('health', {}).get('overall') == 'ready' and
            s.get('health', {}).get('subsystems', {}).get('wake', {}).get('status') == 'ready')
deadline = time.monotonic() + 180
while time.monotonic() < deadline:
    service()
    try:
        s = status()
        if ready(s): break
    except (OSError, ValueError):
        pass
    time.sleep(1)
else:
    raise SystemExit('Full readiness timeout; stop and inspect, do not restart again')
stable_until = time.monotonic() + 10
samples = []
while time.monotonic() < stable_until:
    samples.append(service())
    s = status()
    assert ready(s), s
    time.sleep(1)
samples.append(service())
(p / 'service-loaded-samples.txt').write_text('\n'.join(samples))
(p / 'status-loaded.json').write_text(json.dumps(s, ensure_ascii=False, indent=2))
print(samples[-1])
print(json.dumps(s, ensure_ascii=False, indent=2))
PY
systemctl is-active --quiet firefly-voice-assistant
test "$(systemctl show firefly-voice-assistant -p MainPID --value)" = "$m184_new_pid"
test "$(systemctl show firefly-voice-assistant -p Result --value)" = success
# Confirm this PID actually executes the approved on-disk build.
sudo sha256sum "/proc/$m184_new_pid/exe" | awk '{print $1}' \
  > "$m184_evidence/running-binary.sha256"
cmp "$m184_evidence/approved-binary.sha256" "$m184_evidence/running-binary.sha256"
sudo journalctl -u firefly-voice-assistant --after-cursor="$m184_journal_cursor" \
  --no-pager | tee "$m184_evidence/restart-journal.txt"
grep -F 'SHUTDOWN complete' "$m184_evidence/restart-journal.txt"
grep -F 'RKLLM destroyed' "$m184_evidence/restart-journal.txt"
grep -F 'STARTUP process started' "$m184_evidence/restart-journal.txt"
if grep -E 'Failed with result|Scheduled restart job' "$m184_evidence/restart-journal.txt"; then
    printf '%s\n' 'Failure/automatic restart evidence found; STOP, do not restart again.'
    exit 1
fi
```

Review the captured journal for normal graceful shutdown and startup, with no
crash/restart loop. The stable new PID, successful unit result, complete readiness,
running-image digest and journal together prove the approved process was loaded;
NRestarts is recorded, not assumed to reset or have any particular absolute value.

### A2. Immediate ConfigStore integrity check after loading M18.4

Before any listener/Host/DNS-rebinding/tunnel acceptance, compare the original
pre-restart revision/hash, complete configuration (including `retired_ids`) and
file SHA256. **Any difference means STOP. Do not restore or repair anything.**

```sh
curl --max-time 10 -fsS "$m184_base/api/config" > "$m184_evidence/config-loaded.json"
sudo sha256sum /var/lib/firefly/store/configuration.json > "$m184_evidence/sha-loaded"
cmp "$m184_evidence/sha-before" "$m184_evidence/sha-loaded"
python3 - "$m184_evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
before = json.loads((p / 'config-before.json').read_text())
loaded = json.loads((p / 'config-loaded.json').read_text())
for key in ('revision', 'hash', 'configuration'):
    assert before[key] == loaded[key], f'Production {key} changed during load; STOP, do not restore/repair'
print('M18.4 load preserved original revision/hash and complete configuration including retired_ids')
PY
```

Keep `config-before.json` and `sha-before` untouched for Phase G. Passing this gate
proves loading M18.4 did not mutate the production catalog.

### B–C. Loaded M18.4 listener and Host GET checks

Only after A0–A2 pass, run production network-policy checks against the new process:

```sh
ss -ltnp 'sport = :8080'
curl --max-time 10 -fsS -o /dev/null -w '%{http_code}\n' "$m184_base/"
for m184_path in / /api/status /api/config /api/devices/state; do
    m184_code=$(curl --max-time 10 -sS -o "$m184_evidence/foreign.json" -w '%{http_code}' \
      -H 'Host: attacker.example' "$m184_base$m184_path")
    test "$m184_code" = 403
    python3 - "$m184_evidence/foreign.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
assert r['errors'][0]['code'] == 'http_request', r
assert r['errors'][0]['message'] == 'Host is not allowed.', r
print(r)
PY
done
```

Require `ss` to show only **127.0.0.1:8080**, never wildcard/IPv6/LAN listening on
8080. Normal local GET must return 200. Require microphone connected, wake ready
and WAITING_WAKE in the baseline status. Open Web Admin without editing anything.

### D–E. Isolated negative mutation and bind checks

This single Python block owns its temporary `firefly-admin` editor process from
launch through `Popen.wait`; no cross-shell `wait PID` is used. No model, worker or
voice is started. It can coexist with the production service. The valid POST is
configuration **validation**, which cannot persist changes even if the Host policy
is broken; it is sent only to the isolated editor, never production. Both actual
entrypoints are also checked for invalid bind rejection against fresh `/tmp` stores.
The known LAN example is an explicit LAN IP; optionally replace it with the board's
actual LAN address. No interface is bound by those rejected commands.

```sh
python3 - <<'PY'
import hashlib, http.client, json, pathlib, select, shutil, socket, subprocess, tempfile
repo = pathlib.Path.cwd()
p = pathlib.Path(tempfile.mkdtemp(prefix='firefly-m184-isolated-'))
process = None
port = 18084
print('Isolated evidence directory:', p, flush=True)
try:
    for name in ('firefly-admin', 'firefly-demo'):
        for index, address in enumerate(('0.0.0.0', '192.168.1.20')):
            store = p / f'{name}-{index}'
            store.mkdir(mode=0o700)
            result = subprocess.run([str(repo / 'build' / name), '--store', str(store),
                '--init-config', str(repo / 'config/empty-home.json'),
                '--admin-bind', address, '--admin-port', '0'],
                text=True, capture_output=True, timeout=10)
            assert result.returncode == 1, result
            assert 'V1 Admin bind must be exactly 127.0.0.1' in result.stderr, result.stderr
            assert not list(store.iterdir()), list(store.iterdir())
            print(name, address, 'rejected before store initialization')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', port))  # Stop on conflict; do not kill another listener.
    store = p / 'editor-store'
    store.mkdir(mode=0o700)
    shutil.copyfile(repo / 'config/empty-home.json', p / 'init.json')
    with (p / 'editor.log').open('w') as log:
        process = subprocess.Popen([str(repo / 'build/firefly-admin'), '--store', str(store),
            '--init-config', str(p / 'init.json'), '--admin-bind', '127.0.0.1',
            '--admin-port', str(port)], stdout=subprocess.PIPE, stderr=log, text=True)
    assert select.select([process.stdout], [], [], 10)[0], 'No startup response'
    assert process.stdout.readline().strip() == f'ADMIN_LISTEN http://127.0.0.1:{port}'
    def request(path, host, body=None, origin=None):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        headers = {'Host': host, 'X-Firefly-Admin': '1', 'Content-Type': 'application/json'}
        if origin is not None: headers['Origin'] = origin
        connection.request('GET' if body is None else 'POST', path,
                           None if body is None else json.dumps(body), headers)
        response = connection.getresponse()
        status, data = response.status, json.loads(response.read())
        connection.close()
        return status, data
    code, before = request('/api/config', '127.0.0.1:18080')
    assert code == 200
    digest = hashlib.sha256((store / 'configuration.json').read_bytes()).hexdigest()
    payload = {'configuration': before['configuration']}
    assert request('/api/config/validate', 'localhost:18080', payload, 'http://localhost:18080')[0] == 200
    code, reply = request('/api/config/validate', 'attacker.example', payload, 'http://attacker.example')
    assert code == 403 and reply['errors'][0]['message'] == 'Host is not allowed.', reply
    assert request('/api/config', 'localhost')[1] == before
    assert hashlib.sha256((store / 'configuration.json').read_bytes()).hexdigest() == digest
    print('Matching hostile Host/Origin denied; isolated config unchanged; tunnel Host accepted')
finally:
    if process is not None:
        if process.poll() is None: process.terminate()
        # This process is our child. On timeout, raise and preserve the directory.
        result = process.wait(timeout=15)
        process.stdout.close()
        assert result == 0, f'Editor exit {result}; preserve evidence in {p}'
        assert not pathlib.Path(f'/proc/{process.pid}').exists()
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', port))
# Remove only after successful checks and verified child exit/port release.
shutil.rmtree(p)
print('Isolated acceptance PASS; child exited, port free, temporary directory removed')
PY
```

If a check fails, preserve and inspect the printed temporary directory. If shutdown
times out, do not remove it until the owned process has actually exited; do not use
an unrelated shell's `wait PID`. No production service operation is needed here.

### F. Owner laptop tunnel

In a laptop terminal (replace BOARD with the board's SSH/Tailscale host):

```sh
ssh -N -L 18080:127.0.0.1:8080 firefly@BOARD
```

Keep it running. In a second laptop terminal:

```sh
curl --max-time 10 -fsS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18080/
curl --max-time 10 -fsS http://127.0.0.1:18080/api/status | python3 -m json.tool
curl --max-time 10 -fsS -H 'Host: localhost:18080' http://127.0.0.1:18080/api/status | python3 -m json.tool
```

Open `http://127.0.0.1:18080/` (or `http://localhost:18080/`) and verify Web Admin
loads. Do not Save/Apply. Browser IPv6 localhost resolution depends on the laptop's
SSH listener; the Firefly backend remains IPv4 loopback. Stop the SSH command with
Ctrl+C after checking; this does not stop Firefly.

### G. Production integrity and voice smoke

Back in the original board shell, compare full configuration (including retired IDs),
revision/hash and file SHA256 against the **ORIGINAL A0 pre-restart evidence**
(`config-before.json` / `sha-before`), not the A2 post-load snapshots. This proves
both loading M18.4 and all subsequent security acceptance preserved production
configuration. Any difference means STOP; do not restore or repair anything:

```sh
curl --max-time 10 -fsS "$m184_base/api/config" > "$m184_evidence/config-after.json"
sudo sha256sum /var/lib/firefly/store/configuration.json > "$m184_evidence/sha-after"
cmp "$m184_evidence/sha-before" "$m184_evidence/sha-after"
python3 - "$m184_evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
a, b = (json.loads((p / name).read_text()) for name in ('config-before.json', 'config-after.json'))
for key in ('revision', 'hash', 'configuration'): assert a[key] == b[key], key
print('Production configuration, revision/hash and retired IDs unchanged')
PY
systemctl is-active --quiet firefly-voice-assistant
curl --max-time 10 -fsS "$m184_base/api/status" | python3 -m json.tool
```

Say «Амина», then «Какая погода?». Require wake detection, beep, transcript
«какая погода», safe Unsupported / target_not_grounded on the empty domain and
return to WAITING_WAKE; runtime, microphone, wake and overall health remain ready.
Record the results and log excerpt before removing the evidence directory:

```sh
journalctl -u firefly-voice-assistant -n 150 --no-pager \
  | grep -E 'WAKE_DETECTED|BEEP_START|BEEP_END|VOICE|Unsupported|target_not_grounded'
curl --max-time 10 -fsS "$m184_base/api/status" | python3 -m json.tool
# After recording all evidence:
case "$m184_evidence" in /tmp/firefly-m184-evidence-*) ;; *) exit 1 ;; esac
rm -r -- "$m184_evidence"
```

## Limitations

Host/Origin restrictions are browser network-boundary protections, not authentication.
A local process or an authorized tunnel user can still administer Firefly and supply
allowed headers. A compromised local browser context is not solved by this milestone.
SSH/Tailscale access control remains the owner's external responsibility. Direct
LAN/HTTPS Admin, TLS, accounts, CORS, proxies and firewall changes remain future work.
Real owner acceptance is COMPLETE / PASS as recorded above. The retained runbook
is for repeat verification; no further acceptance was executed during documentation
finalization.
