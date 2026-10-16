# Historical release preparation incident

The following describes the October 2026 rollout #4 preparation, not current production. [Current procedure](../release-procedure.md).

## Rollout #4: established cause and evidence limit

The untracked preparation wrapper added `assert 'NRestarts=1' in props` after
an additional real restart fixture. It attempted to prove `Restart=on-failure`
beyond the already reviewed mandatory tests and typed supervision gate. This
snapshot assertion was not part of a versioned, tested release procedure.
Its failure aborted before production was stopped: downtime was zero.

The first `systemctl show` response was not preserved. Therefore its returned
counter and the immediate reason for the assertion failure remain unknown.
The original journal proves exit 23 followed by PID-1 restart counter 1;
fixture PIDs 497549 and 497570 restored the simulated old release. A later
`LoadState=not-found` / `NRestarts=0` response is not evidence of the first
response. See [incident.json](../evidence/release-procedure/incident.json), including
the original file hashes and precise invocation reference. Originals remain intact.
