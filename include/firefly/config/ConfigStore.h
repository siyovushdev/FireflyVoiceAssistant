#pragma once
#include "firefly/config/Configuration.h"
#include <functional>

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
private:
    std::string directory_;
    StoreFault fault_;
};
}
