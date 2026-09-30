#pragma once
#include "firefly/config/Configuration.h"
#include <functional>
#include <optional>

namespace firefly::config {
struct ExpectedRevision { std::int64_t revision; std::string hash; };
struct StoredRevision { std::int64_t revision; std::string hash; std::int64_t timestampMs = 0; };
// Fault seams run only when a caller explicitly injects a hook (unit tests).
enum class StoreStep { BackupWrite, BackupSync, BackupPublish, BackupDirectorySync,
                       Write, FileSync, Rename, DirectorySync };
using StoreFault = std::function<void(StoreStep)>;
class ConfigStore final {
public:
    explicit ConfigStore(std::string directory, StoreFault fault = {});
    const std::string& directory() const { return directory_; }
    Catalog current() const;
    Catalog initialize(const Catalog& candidate);
    Catalog save(const Catalog& candidate, const ExpectedRevision& expected);
    std::vector<StoredRevision> revisions() const;
    Catalog revision(std::int64_t revision) const;
    Catalog restoreCandidate(std::int64_t revision, const ExpectedRevision& expected) const;
    Catalog restore(std::int64_t revision, const ExpectedRevision& expected);
    // Explicitly reconcile an interrupted transaction; never promotes temp files.
    // A null result means an interrupted initialization left no committed file.
    Catalog reconcile();

    // Last Known Good: configuration that passed ConfigValidator and successful Apply.
    // Durable marker in the store directory; not DeviceState knowledge.
    void recordLastKnownGood(const Catalog& applied);
    std::optional<Catalog> lastKnownGood() const;

private:
    std::string directory_;
    StoreFault fault_;
};
}
