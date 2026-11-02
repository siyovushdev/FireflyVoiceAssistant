# Device telemetry and availability

The existing DeviceStateStore and MQTT observation subscription now retain a
last independent observation separately from a subsequent command prediction.
No ConfigStore schema, command binding, model, inference or transport contract
changes are needed. The [ESP32 example](../examples/esp32-mqtt-led/README.txt)
reports GPIO state every 15 seconds, after connection and after commands.
Periodic reports do not actuate a device. They are non-retained; existing strict
payload validation, arrival/publication barriers and no-replay rules remain.

Availability and power are independent. The existing 60-second observation
freshness policy defines the UI labels:

| Evidence | Availability |
| --- | --- |
| No independent observation yet | Unknown |
| Valid device observation received within 60 seconds | Online, even if power is OFF |
| No observation for more than 60 seconds | No connection: telemetry timeout |
| Observation subscriber disconnected/rejected | No connection: observation transport lost |

These are local observation-based availability estimates, not proof of a
device's power or exact network presence. A command ACK/PUBACK, broker CONNECT,
prediction, malformed/retained sample or browser refresh cannot refresh them.
Broker/Firefly network loss can affect all subscribed devices; the UI explicitly
identifies loss of the observation connection rather than declaring broken
devices. Devices that publish only on change need periodic firmware telemetry
before these labels can be used as a liveness indicator. For HTTP, the current
validated on-demand GET still provides observations; no background poller is
introduced. No fresh HTTP read for 60 seconds likewise means no recent evidence,
not proof the hardware went offline.

Age uses the existing process-local monotonic clock. Epoch timestamps are for
display only. A valid partial state packet proves recent device contact while
each property retains its own independent age. No fake clock reset or TTL
extension is used. All state remains RAM-only: cold restart begins Unknown and
the next live telemetry report repopulates it. No new database or disk writes
on each heartbeat. Periodic telemetry is not historical statistics storage.

On disconnect, current Confirmed/Predicted evidence is revoked exactly as
before, but historical observed value/time are kept separately. A later
prediction cannot overwrite that historical value. Same-mapping in-process
Apply preserves metadata without refreshing age; mapping changes/removal clear
it. Reconcile validates historical values against the new schema as well.

The read-only state API adds device `availability`, `availability_reason`,
`last_observation_at_ms`, `observation_age_ms` and property
`last_observed_value`, `observation_age_ms`. Existing fields retain their meaning.
Web Admin cards, detail and States reuse them, update through the existing
visible-page 10-second snapshot refresh, and fail closed on API/generation
mismatch. No page read sends a device command.

Operational questions use the same store. Fresh state still answers Yes/No;
old state says, for example, “Последнее показание: питание выключено. Сейчас
не могу проверить.” A prediction is never relabeled as a previous observation.
Availability questions such as “Свет онлайн?” use recent device observations,
not power or the publishing broker ACK. Scope/ambiguity/disabled-question guards
and original command text are unchanged. Lists of currently powered devices
still exclude stale knowledge. Historical observations cannot confirm an
execution or silence an unconfirmed command. New dynamic F2 wording can remain
slow; this change does not modify speech synthesis or introduce free chat.

Validation covers steady-time expiry/boundaries, malformed input, prediction
versus observation, disconnect/recovery, retained state rejection, generation
and mapping invalidation, historical OFF/zero, frontend decoding and the actual
example sketch under GPIO/network fakes. Native ESP32 compile/flash and live
broker reception are reported separately from PC tests. Private flash backups
contain credentials and must stay outside Git/public evidence. Permanent
Firefly installation/activation is not implied by this source change.
