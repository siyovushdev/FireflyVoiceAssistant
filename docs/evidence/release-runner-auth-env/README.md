# Release runner authorization isolation

Tested clean code: `a439276a4420e4bd78f2896b1ec97539bb21f3a4`.
The subsequent evidence commit changes documentation only.

Rollout #5 failed because the generic command helper injected live-root
permission into offline CTest. The unmodified ProductionPackaging negative
test correctly exposed it. The helper now strips inherited permission by
default; only its two read-only installed-release verifications opt in.
ProductionPackaging, packaging guards, launcher, recovery, SafetyGate and
voice code remain byte-identical to master `c43bd3b`.

The actual [prepare invocation](prepare-invocation.json) deliberately sets
`FIREFLY_OWNER_ACCEPTED=1` in the parent. Its fixed checks reached
[PREPARED](prepare.stdout): **84/84 CTest**, **40/40 frontend**, typecheck/build,
**17 launcher**, **34 recovery**, **16 runner** tests and **5/5 real systemd
file-only fixtures**, all PASS. The [ProductionPackaging log](production-packaging.log)
records 46/46, including `test_live_roots_and_health_gate`. Three new runner
tests cover an absent/set parent flag, actual child-process isolation, explicitly
scoped permission, and every offline caller's environment.

[verification.json](verification.json) records identities and read-only
production comparisons. PID **467978**, original release, unit, argv, start time,
ConfigStore bytes/rights/timestamps, revision 2/hash `688690…dd18`, backup and
sole NPU/microphone ownership were preserved. An inactive candidate package was
prepared without activation. **No run, cutover, stop/restart, production rollback
timer or new Qwen/audio runtime. Downtime: 0.** Production remains on `dc7b142e`;
the SafetyGate hotfix is still inactive. New deployment requires separate owner
authorization. [Unit observations](unarmed-units.txt) show the prospective
production worker, supervisor and timer were never loaded.

[preparation-commands.jsonl](preparation-commands.jsonl) preserves all 11 actual
commands, stdout, stderr and exit codes: records 0–3 application checks, 4–5
package verifiers, 6 backup, 7–9 operator units, 10 systemd fixtures.
[systemd-fixtures.json](systemd-fixtures.json) contains the five original results.
[provenance.json](provenance.json) links each publication/excerpt to the original
path, SHA256 and selector. Full production snapshots/ConfigStore stay private.
Published logs contain no credentials or production configuration contents.

[Original evidence verification](original-evidence-check.json) checked all
1114 files from rollout #5 without alteration. Its
[failed CTest output](previous-prepare-failure.stdout) remains available for
comparison. Private originals and the new protected backup are at
`/home/firefly/firefly-auth-env-20261012T171101Z/`; the unarmed preparation bundle
is `/run/firefly-release-auth-env-20261012T171101Z/`.

Check the published files with `sha256sum -c SHA256SUMS` in this directory.
This proves offline preparation and environment isolation, not a new production,
real-Qwen or physical-voice acceptance.
