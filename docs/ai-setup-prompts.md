# AI Setup: complete natural-language requests

AI Setup uses the existing local Qwen3-4B W8A8 Admin4096 model. It prepares a private draft. A user reviews the proposed configuration and explicitly applies it; planning does not execute devices.

## Input and context

The application accepts up to 12,000 Unicode code points / 48,000 UTF-8 bytes, including multiline pasted text. This is an input-storage limit, not a promise that every document fits the model context. There is no silent truncation. The existing compiled model context remains 4096 tokens, including instructions, relevant catalog, input and reserved answer.

The former 1800-byte V2 rejection is removed. A complete request is JSON-quoted as `INPUT_DATA` in one admitted Admin context. Chat delimiters are escaped. The user instruction is short and fixed, allowing more of the existing context to hold the document. Real RKLLM token counts and output guards still apply. The ordinary Admin request profile and Runtime1024 remain unchanged. A rejected context restores the previous valid context before a recoverable budget error is reported.

A request's final restrictions remain available to the planner; the UI does not split numbered lists, decimal values, IPv4 addresses or sentences into independent user requests. The legacy explicit stepping API remains for compatibility. The plan is bounded to 12 typed operations. A larger or unsupported plan requires clarification; a prefix must not be presented as complete.

Admission is checked before persisting an incomplete checkpoint. A failed message can be explicitly discarded to its pre-message draft and edited. Crash recovery, owner/version checks, CAS and whole-message rollback remain authoritative. Progress and cooperative Cancel use the existing server session; a running synchronous model call may need to finish before cancellation restores Runtime.

## Connections

A natural-language request can create existing supported logical devices and explicit HTTP, MQTT, Modbus TCP and Modbus RTU connection/mapping/readback contracts. The server allocates IDs and resolves action/device/connection names. The model cannot invent network endpoints, topics, payloads, register addresses, Unit IDs, methods, paths or serial parameters. Missing facts require a question. Credentials are stored using the existing separate protected credentials form; a prompt can refer to an existing `credentials_id`.

Example:

> Создай кухню и свет, который можно включать и выключать. Подключение Брокер — MQTT, host 192.168.68.111, port 1883. Для включения topic home/kitchen/set и payload ON, для выключения тот же topic и payload OFF. QoS 0, retain false. Чтение topic home/kitchen/state, format text, field power, true_payload ON, false_payload OFF.

Specify credentials separately when needed. MQTT AI observations in this MVP use text payloads; configure JSON observations in the existing manual editor. For Modbus supply the zero-based protocol address, function, wire type, Unit ID and TCP endpoint or complete RTU parameters; see [the supported Modbus contract](modbus-connectivity.md). Generic raw TCP/UDP/CAN and arbitrary programmable automation remain unsupported.

## Evidence and limits

Strict schema/domain/grounding validation rejects invalid or unstated operations. Model self-report that a document is complete is not proof of perfect semantic coverage. Review remains necessary. CPU tests with forced model outputs test guards and transactions, not language quality. The opt-in `tests/admin/SetupRealModelTests.py` runs a local Qwen3-4B GGUF CPU backend through the production RuntimeWorker/document decoder to check qualitative examples. It is distinct from native W8A8 RKLLM, performance, resource and hardware acceptance.

The browser tests use the real C++ HttpServer and a private ConfigStore with a scripted inference seam. They cannot establish physical devices or real Qwen behavior. No inference request uses a cloud service.

The document profile has a finite 240-second watchdog per model attempt and at most one additional strict generation when the response is invalid. It never repairs malformed JSON or bypasses validation to accept a result. Cancellation is cooperative: the UI acknowledges the request immediately, but Runtime restoration waits for an in-flight model operation to finish or abort. A valid response is a proposal, not a guarantee that every natural-language request was understood perfectly. If inference repeatedly fails, the original text remains available for editing or manual configuration.
