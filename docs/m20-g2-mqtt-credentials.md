# M20 G2 / Issue #34 — защищённые MQTT credentials (candidate)

**Scope:** Web Admin create/list/rotate/delete external-broker credentials. No system broker provisioning, no accounts in Mosquitto, no production operations. This is a developer candidate, **not** owner acceptance.

## Storage contract

- `/var/lib/firefly/mqtt/credentials` is a **pre-provisioned** absolute directory. Admin has no endpoint to create this path, change its owner or run arbitrary shell commands.
- Every path component is opened from `/` using `openat(O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)`. The final directory must be owned by the effective service UID, mode 0700 (no group/other bits).
- An individual secret is exactly `<id>.cred` with `id = [a-z][a-z0-9_]{0,63}`. Contents are two nonempty UTF-8 lines followed by a newline: MQTT username and password (1–256 bytes each, no ASCII control characters). No JSON body or extra fields on disk. Filenames are generated exclusively from validated IDs.
- Secret files must be owned by the effective service UID, regular, without multiple hardlinks, group/other permissions 0 and readable by the owner (legacy 0400 files and newly created 0600). Symlink/hardlink and invalid content are rejected. An opened valid old inode may report link count 0 after an atomic rename during rotation; it remains a complete, immutable-to-the-reader snapshot and is accepted. Link counts above 1 are rejected.
- New and rotated files are written to random 128-bit-named private temp files (O_EXCL, 0600), fsync'd, then atomically renamed in the same directory. Create uses Linux `renameat2(RENAME_NOREPLACE)`; directory fsync completes durability. Failures are not mistaken for success. A directory fsync failure **after** an atomic rename is explicitly indeterminate and requires a status/re-read check.
- Deletion is refused if the ID is present in stored, active, retained revisions or LKG configuration. Save/CAS reference admission and deletion share the Admin write mutex. API validates referenced secret content at Validate, Save and Apply before activating the runtime.
- The executor and Admin call the **same** read/permission checker; there is no second, weaker credential-reader path.

**Trust boundaries:** This is filesystem access control, **not encryption at rest**. The Admin HTTP server is loopback-bound and uses the existing Host/Origin/custom-header cross-site policy, rather than adding a new login/auth system. Any process with the same service UID and access to the directory is within the trusted boundary. Provisioning must ensure the intended Admin and runtime service identities can read the same private files; don't weaken permissions to solve UID mismatches. Credentials and the directory are not provisioned on the current production Firefly by this branch.

## Admin API

`/api/mqtt/credentials` is an exact, same-origin, no-cache route:

| HTTP method | Request fields | Response | Notes |
| --- | --- | --- | --- |
| GET | none | `{ok:true,ids:[...]}` | Only safe credential IDs; never usernames/passwords |
| POST | `{id,username,password}` | `{ok:true,id,operation:"created",requires_apply:true}` | Create must not overwrite |
| PUT | `{id,username,password}` | `{ok:true,id,operation:"rotated",requires_apply:true}` | Atomic replacement, existing ID only |
| DELETE | `{id}` | `{ok:true,id,operation:"deleted"}` | All retained in-use references block |

The browser clears username/password after the request. Do not embed passwords in a TaskBuffer, MQTT payload, ConfigStore, export, backups, logs, or comments. Generic API error messages do not echo request body.

## G2 R1–R4 independent review corrections

After the first independent review on `8e44c755`, four P2 cases were independently reproduced. The next correction candidate adds:

- **R1 — AI Setup:** `SetupSession::apply` obtains a verifier/commit guard injected by the production Admin API. The guard acquires the same `writes` mutex as credential DELETE and ConfigStore Save/Apply, verifies credential references **before** setup_restore/preflight/Save, and remains held throughout commit/LKG/compensation. Legacy direct SetupSession instances without the injected verifier cannot publish enabled MQTT graphs referencing secrets. A failed check retains the old stored revision and active runtime and leaves the draft editable.
- **R2 — startup/LKG:** `Api::initializeRuntime` selects a current or LKG Catalog only after its own full credential verification and RuntimeGeneration admission. A failed current does not remain truthy; LKG is independently validated. If both fail, boot enters recovery mode with no newly admitted active generation. Boot-time admission is serialized with credential deletion and Save. `/api/config/restore` likewise checks retained revision references before writing a replacement ConfigStore revision.
- **R3 — atomic rotation:** the reader validates type/owner/mode/content on an opened FD and rejects hardlink count above one. A valid old inode unlinked by a concurrent atomic replacement may have `st_nlink == 0`, yet its complete original bytes remain available through that FD; this no longer causes a transient credentials-permissions refusal. No cross-process mutex is assumed.
- **R4 — malicious FIFO:** credential files open with `O_NONBLOCK | O_NOFOLLOW` and are rejected at `fstat` if they are not regular. A FIFO without a writer must not hold the Admin write lock or delay another credential operation.

Updated `MqttCredentialLifecycle` includes file-level FIFO tests, a real `Api.handle` FIFO mutation, 300 parallel rotations with two independent `MqttCredentials` instances, rejection of invalid Setup Save/Apply/LKG, a valid follow-up Setup Apply, startup with both current/LKG credential references unavailable, valid LKG fallback, and malformed secret startup rejection. These are **test definitions, not PASS claims** until built and executed independently.

## Operational limitations

- **Rotate is not broker-side account rotation.** For an external broker, arrange and validate the new account at that broker first; changing Firefly's file does not reconfigure the broker. Existing MQTT sessions may remain on old credentials until reconnect/Apply. Do not automatically reconnect or resend a command as part of rotation.
- The local broker's Firefly service account, common IoT account, password file and safe enable/disable are separate Issue #35, **NOT_IMPLEMENTED** here. No endpoint controls Mosquitto units or ACL.
- Operator directory provisioning, upgrade migration, package owner/mode and rollback are release gate #37, **NOT_VERIFIED** on physical RK3588S.
- This work does not assert the browser/HttpServer credential UX acceptance before independent Chromium tests. Do not claim a release based only on C++ tests or source inspection.

## Isolated test gate

Build with `FIREFLY_HOST_OFFLINE_TESTS=ON`, `FIREFLY_BUILD_ASSISTANT=OFF`, `FIREFLY_BUILD_VOICE=OFF`, `FIREFLY_BUILD_CONFIG_TOOLS=ON`, `BUILD_TESTING=ON`; repeat with MQTT=OFF and with MQTT=ON plus genuine libmosquitto. Run `ctest -R MqttCredentialLifecycle --output-on-failure` plus full safe CTest. Existing MQTT broker and JSON/reconnect tests must not regress; frontend typecheck/tests/build and browser network privacy checks are separate required evidence.

The new native test uses its **own** `mkdtemp` subdirectories, synthetic credentials and a fake CPU ContextModel. No production credentials or systemd services are read or modified. Test cases include file/directory permissions, ID/path validation, symlink/hardlink rejection, 0400 backward compatibility, no-overwrite create, atomic rotation, deletion, retained revision reference protection, API response redaction and missing-secret validation.

**Acceptance state:** implementation committed for independent review. Full G2 #34 acceptance also requires real broker auth/TLS and browser UI tests with documented permission/negative outcomes. Close #34 only after those verifications.
