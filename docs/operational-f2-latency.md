# F2 response latency

The first implementation synthesized every reply, including repeated sentences.
Warm-model hardware measurements at the initial `53429eb` source showed
8.813–10.903 seconds from synthesis start to first playback for common replies.
Those historical numbers exclude recognition, command processing and TTS load.

This change preserves Supertonic3 INT8 F2/SID1, ru, CPU4, five steps, speed1:

- Embed twelve complete synthetic F2 sentences with exact size/SHA256 pins.
  Stock replies bypass both model verification and model loading.
- Brief command receipts say executor acceptance and explicitly distinguish it
  from physical device state. Detailed results retain action/device facts.
- Keep complete plain-text sentences, playing sentence N while synthesizing N+1.
- Reuse only exact-text PCM, maximum 16 chunks / 8 MiB / 20s per entry, volatile
  and scoped to the existing audio lease. Facts and safety are recomputed first.
- Preserve cancellation, finite queues, model admission, Setup drain, generation
  checks, single capture ownership and the acoustic tail.

Build-time bank verification and CPU regressions include corrupted/missing/
symlinked audio, wrong profile, changed text, LRU limits, failed model admission,
cached playback cancellation, next successful turn and absence of SDK calls for
cold stock replies. The native `f2_latency_probe` is an explicitly invoked,
uninstalled playback test; never a CTest or production startup operation.

Hardware measurements and exact build identity are recorded in the task's
private evidence and will be summarized here after execution. New dynamic text
still requires real F2 synthesis; no sub-second guarantee is made for it.
No permanent production installation, release or merge is part of this change.
