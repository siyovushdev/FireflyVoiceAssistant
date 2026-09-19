# nlohmann/json

Unmodified single header from **nlohmann/json v3.12.0**.

- Upstream: https://github.com/nlohmann/json/releases/tag/v3.12.0
- Source: `single_include/nlohmann/json.hpp` at tag `v3.12.0`.
- License: [MIT](LICENSE.MIT), Copyright (c) 2013–2025 Niels Lohmann.
- Header SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`, matching the upstream release checksum.
- Exact source URLs, sizes and hashes: [provenance.json](provenance.json).

Vendored with user approval for Milestone 6.2. CMake verifies the header and
license hashes and uses only this repository copy. There is no download,
package-manager lookup, system installation or remote schema resolution at
build/runtime. The configuration library and Milestone 8 LLM context/structured
protocol module use this dependency privately. GenericAction itself has no JSON
storage/dependency; parser, SafetyGate, STT and VAD do not include JSON. Production
loads config and decodes structured model responses through these modules.

Preserve the header and license verbatim. An upgrade requires an explicit
dependency/version change and verification; do not substitute a system header.
