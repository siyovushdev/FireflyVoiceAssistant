# M19 finalization — 2026-10-14

Closure readiness is based on the explicitly retained **PARTIAL_PASS** boundary.
Real Admin Qwen3-4B ctx4096 initialized, ran five inference calls and produced the
original Кабинет / Лампа proposal with Russian forms and boolean ON/OFF. Review
and one private Apply passed, followed by Runtime/voice restoration. Real human
ON passed microphone → wake → VAD → STT → Runtime → SafetyGate → private POST
→ independent GET. The owner separately confirmed ON/OFF. Instrumented OFF in
the same private window and full ctx4096 stress remain unverified.

The final readiness-GET oracle fix passed offline regression checks; no hardware
trial was repeated afterward or for this closure. Models, thresholds, swap,
production configuration, release and permanent unit are unchanged.

## Source and tested build

The tested executable embeds HEAD `428db2870352e13d43b10ecf97438c78db5aeafb`,
`dirty=true`, built at `2026-10-13T17:17:37Z`. The product sources committed in
`4eca57e` match its 197-file source manifest. The later harness commit `b2cf73f`
contains the PC transport and offline-verified oracle fix. New commit HEADs are
not the executable's embedded HEAD; no new build or hardware result is claimed.

| Identity | SHA-256 |
| --- | --- |
| Candidate ELF | `4f35d9ae2c47d077c4d7e3a8aeb764481d60991b65b5295c203672c32d126abc` |
| Product source | `c04ce42abacfe771eaa480c694d751177720817cc8e3085a4d9fbe01bb1480f5` |
| Admin assets | `fd3cbf4dd6403204f35a8759c8678282d37958854d7654117c5cdc29a01110f9` |

The source fingerprint includes canonical absolute Firefly filenames. Local
verification rebases the PC prefix to the original Firefly source root before
hashing; every file's content is independently compared. Documentation and the
harness are outside this packaged product-source manifest.

## Validation and restoration

The final targeted checks passed 14 native suites and 36 Python tests, including
resource lifecycle, voice restoration, HTTP authorization, private Setup/API,
watchdog/recovery and PC transport. The native group timer expired after 10
passing suites; the remaining four passed separately. This is a targeted gate,
not a repeated full 88-suite run. Grounding passed 450 cases (164 validated POSTs;
286 rejections with zero HTTP). The fresh PC browser offline gate passed Setup,
Review, one Apply, restore and ON/OFF POST/GET using explicitly scripted AI and
voice fixtures. The recorded harmless-GET case passed; early POST was rejected.

Recovery restored the original production service with new PID 1108203,
READY / WAITING_WAKE, sole NPU/microphone owner. That PID remained unchanged
during the final offline and closure checks. Release remained `grounding-818157ec-20261013T040322Z` and
ConfigStore revision 2/hash
`688690e892d2f59413cdc2f7d81f1e129ab9ac0dd247f84b2898d74c3c3cdd18`.
Temporary test services/timers/mocks are inactive; original mock port 8765 remains.
The [curated evidence](evidence/m19/resource-aware-20261014/README.md) preserves
these checks and their scope. Large raw archives and the remote dirty tree are
retained outside Git; no models, audio recordings or recovery credentials are
included in this bundle.

Independent review found no blocking product-code issues within this bounded
acceptance. Local changes are split into product/lifecycle, PC harness, and
qualification/documentation commits. The existing Draft PR #15 update is prepared
locally. Publication requires separate owner permission; no push, merge or rollout
has been performed. Further instrumented OFF/stress qualification would require
new hardware authorization, not an implicit extension of M19 closure.
