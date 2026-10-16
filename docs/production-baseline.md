# M19 production baseline — verified 2026-10-16

This is a dated observation, not a deployment authorization or a live status feed. [README](../README.md) is the entry point; historical milestone/evidence files describe their own dates.

| Identity | Value |
| --- | --- |
| Fetched master / PR #27 merge | `c6e68419eea1f47300f7ca8d51b85997344e405c` |
| Installed immutable release | `m19-c7fa1490-20261015T150156Z` |
| Installed product source | `c7fa14906f8ae15f4c8fb488e9c9d7628eb8154f` |
| Annotated production tag | `m19-production-20261015-c7fa149` → exact product source above |
| Packaged `firefly-demo` SHA256 | `1072ab09a8d43e234a1c1ef58745ab62a8843e1f55dab560c36e7e3c6e7a7be1` |
| Package manifest SHA256 | `23c8909fda60b513f14aae6214a54c71038811277c2215ff4460804ff2918efa` |
| Embedded Admin assets SHA256 | `fd3cbf4dd6403204f35a8759c8678282d37958854d7654117c5cdc29a01110f9` |
| Installed source fingerprint | `a1a840b55c03179718a87ea277a0215a27e69bc8475680fea99867dab63e52db` |
| Build UTC | `2026-10-15T11:48:25Z` |
| Verified rollback release | `m19-320b80a6-20261014T173500Z` |
| Protected Amina anchor | `grounding-818157ec-20261013T040322Z` |

The master merge retains the product commit plus subsequent operator/tests/docs changes. Product source equality was established at publication; a later checkout's HEAD must not be substituted for the installed product's build identity. The existing source fingerprint includes canonical source paths; another checkout may have a different fingerprint despite identical tracked source. Reproducible timestamp control uses `SOURCE_DATE_EPOCH`, and release verification compares exact build, source, manifest and assets pins.

## Installed layout and safe observation

- `/opt/firefly/voice-assistant/current` points to the immutable release.
- `/opt/firefly/voice-assistant/models` holds shared Runtime/Admin/STT/VAD files.
- Amina is a pinned reference into the original grounding release; keep that release.
- `/var/lib/firefly/store` is the active ConfigStore, distinct from repository seeds.
- `firefly-voice-assistant.service` runs the appliance. Admin listens on Firefly loopback port 8080. Current configuration depends on the existing loopback HTTP mock on port 8765; do not stop or repurpose it.
- Snapshot: PID `1456082`, `NRestarts=0`, READY / WAITING_WAKE, ConfigStore revision 2. The public build display label still says “Milestone 17”; use the exact commit and hashes, not that historical label. This cleanup does not rebuild or change the installed application.

Read-only commands on Firefly:

```sh
systemctl show firefly-voice-assistant.service -p ActiveState -p SubState -p MainPID -p NRestarts
readlink -f /opt/firefly/voice-assistant/current
curl --fail --max-time 5 http://127.0.0.1:8080/api/status
```

Do not post test commands, enter Setup, start another assistant, or run hardware harnesses as a status check. Installed `--build-identity` is an early-exit metadata operation; do not replace it with a normal startup.

## Acceptance boundary

Accepted M19: real Admin Qwen3-4B ctx4096; Cabinet/Lamp and Russian forms; boolean power ON/OFF proposal; private Review/Apply; Runtime restoration; instrumented microphone voice ON → STT/Runtime/SafetyGate → POST → independent GET. Owner separately accepted ON/OFF. The later fix passed two bounded Runtime→Admin→Runtime cycles with long requests, no OOM and independent recovery. Maximum observed context was 1211 tokens (1571 reserved), not full 4096 saturation.

Full-context saturation and prolonged soak remain unverified. No new complete machine-instrumented OFF E2E is claimed. Owner voice acceptance remains accepted; these coverage distinctions do not justify silently repeating hardware tests.

Historical qualification: [M19 finalization](m19-finalization.md), [Resource-Aware Setup](m19-resource-aware-setup.md), [bounded diagnostics](m19-admin4096-stress-diagnostics.md). Deployment/publication proof is retained privately on the control PC under `evidence/m19-fix-publication-rollout-20261015T150156Z` and `evidence/m19-final-merge-tag-20261015T161535Z`. Current read-only audit and test logs: `evidence/m19-baseline-20261016`. These private paths are not files in the public repository.

## Recovery boundary

The existing versioned runner, controller, asynchronous launcher and independent systemd rollback helper remain authoritative. Completed rollout plans are consumed, boot-bound historical evidence. There is no permanently armed old rollout watchdog to reuse. A future authorized activation must prepare a fresh verified plan, protected ConfigStore backup, fixed 240-second verification plus 360-second recovery, and verify independent supervision before stopping production. See [release procedure](release-procedure.md). Do not overwrite ConfigStore as a rollback shortcut.
