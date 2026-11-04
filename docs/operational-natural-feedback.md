# Natural operational feedback

The owner requested silence after successful commands and short natural answers
elsewhere. This policy affects voice feedback, not command authorization,
transport acceptance or the configured action payload.

| Evidence/result | Spoken feedback |
| --- | --- |
| Accepted command plus fresh matching independent readback of all assigned fields after this command began | Silence |
| Accepted/PUBACK only, no matching state | Команду отправила, ответа пока нет. |
| Fresh contradictory/partial answer | Устройство ответило, но выполнение не подтвердилось. |
| Delivery timeout/indeterminate result | Не удалось подтвердить выполнение. |
| Unclear/rejected command | Short refusal or clarification |
| One-device positive/negative power-state question with fresh Confirmed data | Да. / Нет. |
| Missing/stale state or failed explicit read | Сейчас не могу проверить. |

The observation window is bounded to 500 ms in total. MQTT consumes only the
existing configured state subscription; it does not invent a request topic,
publish commands, retry or reconnect on a question. HTTP uses the existing
validated observation GET with a stricter remaining timeout, never a longer one.
Scope validation precedes reads. Unknown/ambiguous objects, contradictory
commands, disabled questions, forged model output and Playground never become
read/execute authorization. Model output still supplies only a bounded question
kind; original grounding and command SafetyGate remain authoritative.

Silence requires an actual Accepted execution and matching typed action/target.
Expected fields derive from the configured assignment and actual arguments (or
the validated built-in preview), not state seeds. All expected properties must
be independently Confirmed, unexpired, locally timestamped, ordered after the
command began and equal to the expected values. Failed HTTP readback overrides
cached evidence for this reply. Empty/partial/opposite/future/stale evidence does
not qualify. ExecutionStatus remains Accepted; only feedback reason becomes
state_confirmed. Logs retain the exact execution, state knowledge and read result.
There is no automatic publication when a late observation arrives.

A question may read existing configured observations for at most two targets
(four in detailed mode) within one total budget. MQTT can answer from a fresh
subscription sample; if none exists, it waits only within the budget. Negative
or alternative questions report the state value rather than guessing yes/no
polarity. Single-device yes/no needs no invented gender for configured names.
Property/group/history replies retain configured labels and location phrases.
A past question about whether Amina switched power reports current observed
state without asserting personal causality; last-action questions describe the
recorded dispatch. Failure questions can report that the earlier bounded window
did or did not receive confirmation, explicitly as a past event.

F2/SID1/ru, INT8, CPU4, five steps, speed1 and pinned seven model files are
unchanged. Sixteen complete synthetic short phrases are SHA256-pinned and
embedded. Other freshly grounded text uses the existing bounded exact-text PCM
cache and F2 synthesis. No device facts are cached as decisions. Speech off,
Setup, generation changes, audio stop, single microphone ownership and the
existing playback/echo-tail lifecycle remain in effect. Silent success does not
create a TTS worker, queue audio or incur the speech echo tail.

Tests: OperationalFeedback (typed evidence/temporal/partial/polarity/refresh
admission), OperationalFeedbackHttp (real private HTTP, silence/failed cached
read/timeout/no replay), OperationalFeedbackMqtt (real loopback Mosquitto,
PUBACK-only vs independent state, retained/opposite/late messages, read-only
questions and exact publish counts). Existing independent-review regressions,
frontend and recovery checks remain required. Native and owner acceptance are
reported separately in Draft PR #43; these test descriptions are not PASS claims.
