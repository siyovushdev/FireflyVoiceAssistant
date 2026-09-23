# Milestone 12 — Local Admin Copilot

Production remains Qwen3-4B W8A8 RK3588, context **1024**. No 4096 work, no model/export
change, no HardwareExecutor, no voice input in this milestone.

## Architecture

```text
natural language
      ↓
scoped inventory + fixed Copilot prompt (fail-closed if inventory >900 bytes)
      ↓
structured JSON {status, summary, operations[]}
      ↓
strict decode (one bounded retry for malformed JSON only)
      ↓
deterministic apply onto COPY of current Admin draft
      ↓
ConfigValidator
      ↓
candidate + configurationDiff + base_hash
      ↓
human accept → local draft only
      ↓
existing Validate → Save → Apply
```

Operations (bounded): `create_location`, `create_device`, `rename_location`,
`add_location_mention`, `add_location_phrase`, `delete_device`, `set_field_bounds`.
At most 2 new locations and 6 new devices per proposal. Multi-location atomic
requests are supported within that bound; partial execution is rejected.

Creating new custom typed capabilities (extra state fields without template
actions) is a documented limitation of the next stage. Existing template
capabilities and changing bounds on existing numeric fields remain in scope.

`AdminPlan` (single-location create) remains available for the older “Описать,
что добавить” wizard. Copilot is the generalized NL path over the **current draft**.

## Stale / ambiguity / door

- Proposal carries `base_hash` = content hash of the submitted draft.
- `/api/admin/copilot/accept` rejects when the current draft hash ≠ `base_hash`
  (`stale_proposal`). No silent merge.
- LLM `status=ambiguous|unsupported` returns a controlled rejection without
  modifying configuration (not treated as malformed JSON retry).
- Protected entrance door cannot be created as `door` or as a semantic switch.

## HTTP

- `POST /api/admin/copilot` `{text, configuration, draft_hash?}`
- `POST /api/admin/copilot/accept` `{configuration, proposal}`

Both keep `saved=false`, `applied=false`. Metrics come from the existing RKLLM
admin suggestion path (context/input/output tokens, budget, wall, retry).
Copilot uses response budget **384** (ceiling 512 on context 1024). Inventory is
compact and fail-closed above **900** bytes.

Missing `mentions`/`locations`/`aliases` are filled with deterministic defaults
from the entity name. Template aliases such as `air_conditioner` map to `switch`.

## UI

Simple mode section **Помощник по настройке**: text field, propose, status,
human summary, diff lines, accept-to-draft, cancel. Advanced shows raw proposal.

## Known limitations

- Not a general chat bot; only structured configuration operations.
- Multi-location create bounded to 2 locations / 6 devices per proposal.
- Default speech forms from a bare name may be grammatically imperfect; user edits draft.
- Protected entrance door remains outside Copilot create/delete.
- Large drafts that exceed the scoped inventory budget are rejected (`context_overflow`).
- New custom typed capabilities (state field + executable action/parameter wiring)
  are deferred; use template devices or edit existing numeric field bounds.
