# M18.5 — Config & State Time Hardening

Baseline `661bc1cebb0771d933431bd413bfd25700de065b`; branch
`hardening/config-state-time`. M18.1–M18.4 remain COMPLETE / PASS.
Owner acceptance: **COMPLETE / PASS**. This milestone only bounds configuration history
and separates device freshness/order from wall time.

## ConfigStore: newest 64 historical backups

Previously every committed predecessor accumulated indefinitely in `backups/`;
ConfigStore tests explicitly retained more than ten revisions. The production
contract now exposes `kMaxHistoricalBackups = 64` in `ConfigStore.h`.

`configuration.json` is current, not one of the 64. The retained set is the newest
64 historical committed revisions, selected by **validated revision**, never mtime.
The existing revisions API shape/order is unchanged; it naturally lists only retained
history. Restore of a retained revision creates a new revision with the current CAS
and retirement rules. A pruned revision returns existing `unknown_revision`.
No archive tier, retention setting or UI control is added. The B4 UI says “Версии”
and does not promise all past revisions, so no frontend/bundle edit is needed.

### Ordering and durability

All mutation paths still hold the existing ConfigStore flock. `commit()`:

1. Validates the proposed next revision and identity/retirement rules.
2. Opens/validates the backup directory and **validates all committed backup entries**
   through `listBackups`: canonical catalog, filename equal to `<revision>-<hash>.json`,
   one identity per revision, regular single-linked file, correct owner/mode and
   `O_NOFOLLOW`. Future/conflicting predecessor revisions fail closed.
3. Computes space needed for the current configuration's predecessor snapshot.
   A snapshot already present after an aborted save consumes no extra slot. Legacy
   over-cap history is handled in the same pass. Only the oldest excess identities
   are selected; the immediate predecessor is never the eviction target.
4. If an eviction would remove a legacy Last Known Good fallback, makes the dedicated
   LKG snapshot durable first, as described below.
5. Unlinks only selected validated backup filenames via the backup directory fd,
   then fsyncs that directory **before** creating/publishing the predecessor backup.
6. Continues the existing durable predecessor → `.pending.json` → current-file
   publication → pending-marker removal sequence unchanged.

Successful mutations leave at most 64 historical backups. A pruning/migration failure
before the configuration transaction aborts the mutation: current bytes, hash and
revision remain unchanged, no pending transaction starts, and no success is reported.
Some oldest backups may already have been removed if a later unlink/fsync fails;
those deletions are not rolled back. This is not a post-commit cleanup that can
report an ordinary failure after current has changed. A failed subsequent save can
also leave a validated backup of the still-current revision, as before; retry accounts
for it without pruning an additional slot unnecessarily.

The current file, pending marker, lock and both LKG files are never pruning targets.
Uncommitted `.tmp-*` artifacts remain ignored/preserved under the existing crash
rules; they are not committed revisions and are not blindly deleted. Existing
interrupted-transaction reconcile never promotes temp files. Retention is enforced
on the next save/restore/compensation, not by a background task or read-only listing.

### Legacy Last Known Good

`last_known_good_snapshot.json` remains outside the history cap. For an older
`last_known_good.json` marker that points to an ordinary backup about to be pruned,
retention first publishes the exact validated catalog to the existing dedicated
snapshot and fsyncs it and the store directory. The marker's identity is preserved.
A missing, stale-identity or malformed regular snapshot can be repaired from that
verified backup; unsafe objects and I/O errors fail closed.

An existing matching snapshot is also re-synced before pruning: it might have been
published by an earlier attempt that failed its directory fsync. A migration failure
leaves the fallback backup untouched and never starts the configuration transaction.
The marker + dedicated snapshot can still recover LKG after the ordinary revision
is pruned, even if current is later corrupt. There is no permanent over-cap pin.
Missing/unrecoverable LKG history is not invented; existing recovery semantics remain.

Compensation reads the exact immutable immediate predecessor under saved CAS, then
commits a new revision. It still reactivates only predecessor identities, preserves
prior retired IDs and retires IDs created by the failed setup transaction.

## Device state: two distinct clock domains

### External timestamp audit and compatibility decision

The old `SteadyClockMs` alias/default clock actually returned `system_clock` time.
Property TTL subtracted `updatedAtMs` or `observedAtMs`; observation ordering compared
those same wall timestamps. Clock adjustments could distort freshness and reject a
new local observation after a backward wall jump.

The timestamps are **not purely internal**:

| Field | Existing consumer and final semantics |
| --- | --- |
| `PropertyKnowledge.updatedAtMs`, `observedAtMs` | RuntimeWorker serializes `updated_at_ms` / `observed_at_ms`; frontend `adapters/catalog.ts` maps them to property display timestamps and `formatTime` uses `new Date(...)`. Remain Unix/wall milliseconds. |
| `DeviceKnowledge.lastSuccessAtMs`, `lastFailureAtMs` | Serialized connectivity event timestamps; `lastSuccess` is exposed to the frontend. Remain Unix/wall milliseconds. No connectivity TTL/age is currently calculated. |
| `ExecutionResult.timestampMs` | Serialized execution event timestamp. Remains Unix/wall milliseconds. Runtime generation history also retains its existing wall event time for Overview/log display. |
| `StateObservation.timestampMs` | Trusted internal observation event/display timestamp; HTTP poll uses local wall time. No public observe-injection endpoint exists since M18.3. Event time no longer authorizes ordering or freshness. |
| `/api/devices/state`, dashboard | Existing timestamp fields and JSON shape unchanged; derived `stale` now uses steady time. |

No public timestamp silently becomes milliseconds since boot. Display timestamps
may move backward after an operator/NTP adjustment, as real wall time does; derived
freshness and local ordering do not. No frontend date conversion or API schema change
is required.

### Freshness and local ordering

DeviceStateStore takes separately injectable steady and wall clock functions.
Defaults use `std::chrono::steady_clock` and `std::chrono::system_clock`, respectively.
`nowMs()` remains the wall/event clock; `steadyNowMs()` is the process-local clock.
The new property metadata is never serialized:

- `freshnessSteadyMs`: captured when accepted prediction/observation is recorded.
- `orderSteadyMs`: local order of the evidence, used to reject older observations.

Predictions sample the steady clock on acceptance. HTTP poll captures a local order
stamp **before its GET** and records wall time on completion. A delayed GET cannot
use its later wall timestamp to override a newer local prediction. Successful
observation admission stamps freshness at receipt. Validation/order checks complete
for the whole property map before any property changes. A newer contradictory local
observation can replace prediction/confirmation even when wall time has moved backward.
Older steady order is rejected with `stale_observation`; equal millisecond stamps
retain the previous equal-timestamp acceptance convention.

`StateObservation.orderSteadyMs` is explicitly separate from `timestampMs`.
A synchronous local observation may omit both (event timestamp zero): admission
samples the two local clocks. An explicit event timestamp must be accompanied by
a non-future **same-process steady order stamp**, otherwise `invalid_observation`
is returned. Existing isolated typed-observation tests now provide both domains.
A future transport's remote source timestamp alone cannot prove local order; that
transport must validate its own source ordering and capture/associate a local stamp,
not translate an unrelated remote clock into this field. No such transport is added.

The internal RuntimeGenerations seam no longer pre-fills a wall timestamp before
admission; synchronous observations are stamped in DeviceStateStore. The public
M18.3 route remains absent. Failed execution/polling still cannot create observation
evidence or refresh a property's age. Reachability event timestamps remain display
values only; a failure can change connectivity without changing knowledge.

TTL remains **60,000 ms**, with stale iff known property age is **strictly greater**
than TTL. A steady timestamp of zero is valid (optional metadata distinguishes absence).
Wall jumps by hours/days in either direction have no freshness effect. Compatible
in-process Apply copies the steady metadata without refreshing it. Mapping invalidation
clears it; a new process still starts Unknown. No durable telemetry is introduced.

## Automated verification

- `ConfigRetention` references the production cap and tests real saves across the
  boundary, exact revision ranges, reversed mtimes, legacy overflow, retained/pruned
  restore, retirement and setup compensation.
- It injects unlink and directory-sync failures before configuration mutation;
  verifies exact current bytes/hash/revision and no pending marker/new predecessor;
  exercises retry plus crash/reconcile at prune/write/publication boundaries.
- Legacy LKG tests cover no snapshot, mismatched snapshot, malformed snapshot,
  migration sync failure/retry, pruning, and recovery with corrupt current.
- Security cases cover unexpected names, symlinks, directories, unsafe mode and
  malformed backup JSON; no unvalidated entry is blindly deleted.
- Existing ConfigStore fault, concurrency and interrupted initialization tests remain.
- `DeviceStateLifecycle` injects independent clocks with **no sleeps for clock tests**:
  actual default clock selection, fresh and >60s Predicted/Confirmed, exact boundary,
  zero steady origin, forward and
  backward wall jumps, steady-only aging, Apply age retention, stale local observation
  with future wall time, event-only rejection, newer contradictory evidence with older
  wall time, connectivity failure, Unix execution timestamps and cold Unknown.
- API/UI/startup/recovery suites retain CAS, restore, compensation, M18.3 polling,
  M18.4 Host/bind policy and Chromium coverage.

```sh
cmake -S . -B build -DRKLLM_ROOT=/home/firefly/demo_Linux_aarch64
cmake --build build -j2
ctest --test-dir build -R 'Config|DeviceStateLifecycle|HardwareBindings|HealthReliability|ProductionApiHardening|AdminNetworkSecurity|AdminHttp|AdminManualApi|AdminGuardedCommand|AdminSetup|ProductionStartup|VoiceRecovery' --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

Results (2026-10-09):

- Configure/build PASS; final build has no compiler warnings.
- New/baseline core checks: **ConfigStore, ConfigRetention, DeviceStateLifecycle 3/3 PASS,
  26.90 s**. Retention completed **1,054 checks**. Its hand-built legacy fixture was
  corrected to private file/directory modes after the security guard correctly rejected
  permissions inherited from the shell umask; production security was not relaxed.
- Focused: **25/25 PASS, 138.23 s**. The final real-default-clock assertion also passed
  in a subsequent DeviceStateLifecycle run (**0.56 s**), before full CTest.
- Full CTest: **76/76 PASS, 312.28 s**, including AdminV2Chromium (**62.27 s**),
  AdminManualChromium (**46.27 s**) and AdminSetupV2Chromium (**43.32 s**).
- `git diff --check` PASS; new documentation also passed whitespace checks.
- Owner runbook: eight shell blocks / four embedded Python blocks syntax-checked only.

Logs: `/tmp/firefly-m185-configure.log`, `/tmp/firefly-m185-build-final.log`,
`/tmp/firefly-m185-test-fix-build.log`, `/tmp/firefly-m185-clock-test-build.log`,
`/tmp/firefly-m185-new-tests.log`, `/tmp/firefly-m185-focused.log`,
`/tmp/firefly-m185-clock-final.log`, `/tmp/firefly-m185-ctest.log`.
Implementation verification used no production store, installed service operations,
host-clock changes or owner hardware acceptance. The subsequent owner-performed
acceptance is recorded below; this documentation finalization executes none of it.

## Real owner acceptance — COMPLETE / PASS

### 1. Automated verification — PASS

Implementation verification remains applicable: **ConfigRetention 1,054 checks PASS**,
**focused CTest 25/25 PASS**, **full CTest 76/76 PASS including Chromium**, and
**git diff --check PASS**. The final owner deterministic rerun also passed:

| Suite | Checks | Result | Duration |
| --- | --- | --- | --- |
| ConfigRetention | 1,054 | PASS | 23.80 s |
| DeviceStateLifecycle | 82 | PASS | 0.52 s |
| Combined | 2/2 suites | PASS | 24.33 s |

Clock tests used injected deterministic clocks. The system clock was not modified.
These tests provide the time-jump, ordering, compensation and legacy LKG evidence;
the physical acceptance below does not claim to exercise host-clock jumps.

### 2. Production load — PASS

Before loading M18.5, production had MainPID=212461, NRestarts=0,
ActiveState=active, runtime_ready=true, voice=WAITING_WAKE, microphone=connected,
health=ready and wake=ready.

- Old running executable SHA256:
  `21ee9c9c7d8d1971e6cd9329d6371ed62b5a8da32d6dd9fe6c2a22cfd5bbafb7`.
- Approved M18.5 disk executable SHA256:
  `ba03c8ed5aa701be06898abd47be3bddc3190a065aca1fee27f6b8415fc86a07`.

The owner performed **exactly one controlled systemd restart**. Afterwards:
MainPID=292014, NRestarts=0, ActiveState=active, Result=success,
runtime_ready=true, voice=WAITING_WAKE, microphone=connected, overall=ready and
wake=ready. The running executable SHA256 matched the approved M18.5 build exactly:
`ba03c8ed5aa701be06898abd47be3bddc3190a065aca1fee27f6b8415fc86a07`.

The journal confirmed clean shutdown, RKLLM destroyed, a new process started,
Admin listening, runtime ready, WAITING_WAKE and overall ready.

### 3. Isolated backup retention — PASS

The owner used the real `build/firefly-config` binary, `config/empty-home.json`
and a separate temporary ConfigStore under `/tmp`. Production configuration was
not mutated. Initialization created revision 1, followed by 70 sequential Save
operations:

- Current revision=71; historical backup count=64.
- Oldest retained revision=7; newest retained revision=70.
- Restore of pruned revision 1 returned `unknown_revision`; current configuration
  bytes remained unchanged.
- Restore of retained revision 7 succeeded as new current revision=72.
- Historical backup count remained 64, with retained revisions 8..71 and
  `retired_ids=[]`.

All acceptance assertions passed. The temporary test store was removed only after
successful checks.

### 4. State time and ordering — PASS

DeviceStateLifecycle deterministic checks verified public Unix/wall timestamp
semantics, monotonic freshness and local ordering, the unchanged 60-second default
TTL, out-of-order observation protection, Predicted → Confirmed semantics and safe
failure behavior. Wall-clock jumps were represented by injected clock values;
**no physical system-clock modification was performed**.

### 5. Final production integrity — PASS

Production before and after acceptance matched exactly:

| Evidence | Original and final value |
| --- | --- |
| Revision | 1 |
| Configuration hash | `36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be` |
| `configuration.json` SHA256 | `36e6c9c3ffe311a2d0b84de69ce48cc0d84051ce1a424b37397de925c06af0be` |
| `retired_ids` | `[]` |
| Backup history count | 0 |

Revision, hash, complete configuration, SHA256, history and retired IDs were all
unchanged. The M18.4 regression also passed: listener only `127.0.0.1:8080`, and
foreign `Host: attacker.example` returned HTTP 403.

Production remained MainPID=292014, NRestarts=0, ActiveState=active,
runtime_ready=true, voice=WAITING_WAKE, microphone=connected, overall=ready and
wake=ready.

### 6. Final real voice smoke — PASS WITH OBSERVATION

The first recorded utterance reached the existing maximum speech duration of
approximately 15 seconds. Wake, beep and listening proceeded, SPEECH_START occurred,
and the journal reported **"Maximum speech duration exceeded; discarding until silence"**.
No STT inference was performed for that utterance; the service returned safely to
WAITING_WAKE. This was a safe rejection, not a process crash, and is recorded as a
**separate non-blocking audio/VAD observation**. Not every attempted utterance succeeded.
VAD/voice code was not changed in M18.5; recurring maximum-duration failures should
be investigated separately.

A subsequent real voice attempt fully passed:
WAKE_DETECTED → BEEP_START → BEEP_END → LISTENING_COMMAND → SPEECH_START →
SPEECH_END → PROCESSING. The STT transcript was exactly **"какая погода"**.
The empty production domain returned `unsupported=true` / `target_not_grounded`,
with **no hardware execution**, then returned to WAITING_WAKE.

Final status: MainPID=292014, NRestarts=0, runtime_ready=true,
voice=WAITING_WAKE, microphone=connected, last_voice.transcript="какая погода",
overall=ready and wake=ready.

### Final M18.5 conclusion

Owner acceptance is **COMPLETE / PASS**. ConfigStore history is bounded to 64
historical backups; the current configuration is outside the cap and dedicated LKG
is preserved. Pruned revisions return `unknown_revision`; retained restore and
compensation remain supported. Wall-clock changes do not govern state TTL, while
public Unix timestamp semantics remain intact. Existing API and SafetyGate behavior
are unchanged. Real production configuration was not modified, and production
runtime/voice recovered healthy after the controlled restart. The first overlong
voice utterance remains documented as a separate non-blocking observation.

## Owner acceptance runbook — retained for reproducibility

Use the approved build after review. The owner, not the coding agent, runs this
procedure. Production is read-only apart from **one controlled restart if the running
image differs from the approved binary**. Never create 65+ production revisions,
Save/Apply/import/restore production, edit the unit, change the clock, or repair a
failed integrity comparison. Stop on any failure. Firmware prerequisite remains
XVF3800 >=2.0.10 (2.0.10 directly validated).

### A. Original production baseline

In one board Bash session, keep the following evidence until final comparison:

```sh
cd ~/FireflyVoiceAssistantLinux
set -euo pipefail
umask 077
m185_evidence=$(mktemp -d /tmp/firefly-m185-evidence-XXXXXX)
m185_base=http://127.0.0.1:8080
systemctl is-active --quiet firefly-voice-assistant
systemctl show firefly-voice-assistant -p MainPID -p NRestarts -p ActiveState -p Result \
  | tee "$m185_evidence/service-before.txt"
m185_old_pid=$(systemctl show firefly-voice-assistant -p MainPID --value)
test "$m185_old_pid" -gt 0
sudo readlink "/proc/$m185_old_pid/exe" | tee "$m185_evidence/exe-before.txt"
sudo sha256sum "/proc/$m185_old_pid/exe" | awk '{print $1}' > "$m185_evidence/binary-before.sha"
curl --max-time 10 -fsS "$m185_base/api/status" > "$m185_evidence/status-before.json"
curl --max-time 10 -fsS "$m185_base/api/config" > "$m185_evidence/config-before.json"
curl --max-time 10 -fsS "$m185_base/api/config/revisions" > "$m185_evidence/history-before.json"
sudo sha256sum /var/lib/firefly/store/configuration.json > "$m185_evidence/sha-before"
python3 - "$m185_evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
s = json.loads((p / 'status-before.json').read_text())
assert (s['runtime_ready'] and s['voice'] == 'WAITING_WAKE' and s['microphone'] == 'connected'
        and s['health']['overall'] == 'ready' and s['health']['subsystems']['wake']['status'] == 'ready'), s
c = json.loads((p / 'config-before.json').read_text())
print('Original revision/hash:', c['revision'], c['hash'])
print('Original retired_ids:', c['configuration']['retired_ids'])
print(json.dumps(s, ensure_ascii=False, indent=2))
PY
```

### B. Load approved binary and verify integrity before isolated tests

Do not rebuild during acceptance. A changed disk executable does not replace a
running image. Use the existing installed unit, without reinstall/edit/daemon-reload.

```sh
test -x "$PWD/build/firefly-demo"
sha256sum "$PWD/build/firefly-demo" | awk '{print $1}' > "$m185_evidence/approved.sha"
m185_cursor=$(sudo journalctl -u firefly-voice-assistant -n 1 --show-cursor --no-pager \
  | sed -n 's/^-- cursor: //p')
test -n "$m185_cursor"
m185_restarted=false
if ! cmp -s "$m185_evidence/approved.sha" "$m185_evidence/binary-before.sha"; then
    sudo systemctl restart firefly-voice-assistant
    m185_restarted=true
fi
m185_pid=$(systemctl show firefly-voice-assistant -p MainPID --value)
test "$m185_pid" -gt 0
if test "$m185_restarted" = true; then test "$m185_pid" != "$m185_old_pid"; fi
python3 - "$m185_base" "$m185_pid" <<'PY'
import json, subprocess, sys, time, urllib.request
base, pid = sys.argv[1:]
end = time.monotonic() + 180
stable_since = None
while time.monotonic() < end:
    current = subprocess.check_output(['systemctl', 'show', 'firefly-voice-assistant', '-p', 'MainPID', '--value'], text=True).strip()
    assert current == pid, 'Unexpected process restart; stop acceptance'
    try:
        with urllib.request.urlopen(base + '/api/status', timeout=2) as response: s = json.load(response)
        ready = (s.get('runtime_ready') is True and s.get('voice') == 'WAITING_WAKE' and
                 s.get('microphone') == 'connected' and s.get('health', {}).get('overall') == 'ready' and
                 s.get('health', {}).get('subsystems', {}).get('wake', {}).get('status') == 'ready')
    except (OSError, ValueError): ready = False
    if ready:
        if stable_since is None: stable_since = time.monotonic()
        if time.monotonic() - stable_since >= 10:
            print(json.dumps(s, ensure_ascii=False, indent=2)); break
    else: stable_since = None
    time.sleep(1)
else: raise SystemExit('Full readiness timeout; stop, do not restart again')
PY
systemctl is-active --quiet firefly-voice-assistant
systemctl show firefly-voice-assistant -p MainPID -p NRestarts -p ActiveState -p Result \
  | tee "$m185_evidence/service-loaded.txt"
sudo sha256sum "/proc/$m185_pid/exe" | awk '{print $1}' > "$m185_evidence/running.sha"
cmp "$m185_evidence/approved.sha" "$m185_evidence/running.sha"
sudo journalctl -u firefly-voice-assistant --after-cursor="$m185_cursor" --no-pager \
  | tee "$m185_evidence/load-journal.txt"
if test "$m185_restarted" = true; then
    grep -F 'SHUTDOWN complete' "$m185_evidence/load-journal.txt"
    grep -F 'RKLLM destroyed' "$m185_evidence/load-journal.txt"
    grep -F 'STARTUP process started' "$m185_evidence/load-journal.txt"
fi
```

Require normal journal shutdown/startup, no crash loop and Result=success. Record
NRestarts without assuming a particular reset value. Define and run this comparison
against the **original** evidence; reuse it in E. Any difference means STOP, not restore.

```sh
m185_integrity() {
    curl --max-time 10 -fsS "$m185_base/api/config" > "$m185_evidence/config-after.json"
    curl --max-time 10 -fsS "$m185_base/api/config/revisions" > "$m185_evidence/history-after.json"
    sudo sha256sum /var/lib/firefly/store/configuration.json > "$m185_evidence/sha-after"
    cmp "$m185_evidence/sha-before" "$m185_evidence/sha-after"
    python3 - "$m185_evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
a, b = (json.loads((p / name).read_text()) for name in ('config-before.json', 'config-after.json'))
for key in ('revision', 'hash', 'configuration'): assert a[key] == b[key], key
assert json.loads((p / 'history-before.json').read_text()) == json.loads((p / 'history-after.json').read_text())
print('Original production configuration, retired IDs, history and SHA256 unchanged')
PY
}
m185_integrity
```

### C. Real isolated ConfigStore retention

This uses the production `firefly-config` CLI and real ConfigStore commits in a new
private `/tmp` directory, with the empty repository seed. It uses no server, model,
production port, systemd operation or production store. All child invocations are
synchronous; failures preserve the directory for inspection.

```sh
python3 - <<'PY'
import json, pathlib, shutil, subprocess, tempfile
repo = pathlib.Path.cwd()
p = pathlib.Path(tempfile.mkdtemp(prefix='firefly-m185-retention-'))
store = p / 'store'; store.mkdir(mode=0o700)
seed = p / 'seed.json'; shutil.copyfile(repo / 'config/empty-home.json', seed)
print('Isolated store:', store, flush=True)
def run(command, *args, expected_code=None):
    result = subprocess.run([str(repo / 'build/firefly-config'), command, '--store', str(store), *map(str, args)],
                            text=True, capture_output=True, timeout=30)
    if expected_code:
        assert result.returncode == 1, result
        reply = json.loads(result.stderr); assert reply['code'] == expected_code, reply
        print('Expected error:', expected_code); return reply
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)
def cas(c): return ['--expected-revision', c['revision'], '--expected-hash', c['content_sha256']]
current = run('init', '--file', seed)
for _ in range(70): current = run('save', '--file', seed, *cas(current))
assert current['revision'] == 71
entries = run('revisions')['revisions']
assert len(entries) == 64 and [e['revision'] for e in entries] == list(range(7, 71)), entries
assert len(list((store / 'backups').glob('*.json'))) == 64
print('Current 71; exactly 64 historical revisions: 7..70')
before = (store / 'configuration.json').read_bytes()
run('restore', '--revision', 1, *cas(current), expected_code='unknown_revision')
assert (store / 'configuration.json').read_bytes() == before
current = run('restore', '--revision', 7, *cas(current))
assert current['revision'] == 72
assert [e['revision'] for e in run('revisions')['revisions']] == list(range(8, 72))
assert len(list((store / 'backups').glob('*.json'))) == 64
assert json.loads((store / 'configuration.json').read_text())['retired_ids'] == []
print('Retained restore created revision 72; history now 8..71; pruned restore rejected')
shutil.rmtree(p)
print('Isolated retention PASS; temporary store removed')
PY
```

### D. Time behavior without changing the host clock

The deterministic injected-clock tests are authoritative evidence. Optionally rerun
only the owner-visible lifecycle binary, which uses temporary configuration/fake HTTP
and no NPU or microphone; its wall-jump assertions modify variables, not the host:

```sh
./build/src/admin/device_state_tests "$PWD"
```

Require PASS. Do not run `date -s`, timedatectl changes or NTP operations. No live
clock manipulation is necessary for this milestone.

### E–F. Final integrity and production smoke

Back in the original shell, compare against A again, then verify Web Admin and the
M18.4 policy. The foreign-Host probe is GET-only and cannot mutate configuration.

```sh
m185_integrity
systemctl is-active --quiet firefly-voice-assistant
test "$(systemctl show firefly-voice-assistant -p MainPID --value)" = "$m185_pid"
ss -ltnp 'sport = :8080'
curl --max-time 10 -fsS -o /dev/null -w '%{http_code}\n' "$m185_base/"
m185_code=$(curl --max-time 10 -sS -o "$m185_evidence/foreign.json" -w '%{http_code}' \
  -H 'Host: attacker.example' "$m185_base/api/status")
test "$m185_code" = 403
curl --max-time 10 -fsS "$m185_base/api/status" | python3 -m json.tool
```

Require listener only `127.0.0.1:8080`, Web Admin 200, runtime/microphone/wake ready
and WAITING_WAKE. Say «Амина» → beep → «Какая погода?»; require exact transcript
«какая погода», safe Unsupported / target_not_grounded and return to WAITING_WAKE.
Record final status and journal, then remove only the acceptance evidence directory:

```sh
journalctl -u firefly-voice-assistant -n 150 --no-pager \
  | grep -E 'WAKE_DETECTED|BEEP_START|BEEP_END|VOICE|Unsupported|target_not_grounded'
curl --max-time 10 -fsS "$m185_base/api/status" | python3 -m json.tool
# After recording acceptance evidence:
case "$m185_evidence" in /tmp/firefly-m185-evidence-*) ;; *) exit 1 ;; esac
rm -r -- "$m185_evidence"
```

## Limits and compatibility

Older ordinary revisions intentionally become unavailable; dedicated LKG is not a
user-visible revision archive. Legacy excess history is bounded at its next mutation.
Retention bounds committed revision count, not configuration size, temporary crash
artifacts or disk consumption by unrelated files. Filesystem durability still depends
on storage honoring fsync. No malicious same-UID writer outside the store lock is
supported. Private steady stamps are valid only within this process and clock domain;
future transports must establish their own source-order contract. No persistent
telemetry, clock management, packaging, authentication or new transport work is added.
