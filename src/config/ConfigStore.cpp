#include "firefly/config/ConfigStore.h"
#include "firefly/config/ConfigLoader.h"
#include "firefly/config/ConfigValidator.h"
#include "JsonInternal.h"
#include "Platform.h"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace firefly::config {
namespace {
using detail::FileDescriptor;
constexpr char CurrentFile[] = "configuration.json";
constexpr char PendingFile[] = ".pending.json";
constexpr char LastKnownGoodFile[] = "last_known_good.json";
constexpr char LastKnownGoodSnapshot[] = "last_known_good_snapshot.json";
void secure(int descriptor, bool directory) {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) detail::ioError("stat store");
    if ((directory ? !S_ISDIR(status.st_mode) : (!S_ISREG(status.st_mode) || status.st_nlink != 1)) ||
        status.st_uid != ::geteuid() || (status.st_mode & 0022) != 0)
        throw ConfigError("unsafe_store", "", "Store must be owned by this user, without group/other write access or linked special files");
}
uid_t secureForInspect(int descriptor, bool directory) {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) detail::ioError("stat store");
    if ((directory ? !S_ISDIR(status.st_mode) : (!S_ISREG(status.st_mode) || status.st_nlink != 1)) ||
        (status.st_mode & 0022) != 0)
        throw ConfigError("unsafe_store", "", "Store must be a private regular file or directory, without group/other write access or linked special files");
    if (status.st_uid == ::geteuid()) return status.st_uid;
    // Root may read a non-root private store during owner cutover. It must not write.
    if (::geteuid() == 0 && status.st_uid != 0) return status.st_uid;
    throw ConfigError("unsafe_store", "", "Store must be owned by this user, without group/other write access or linked special files");
}
struct LockedStore {
    FileDescriptor directory, lock;
    explicit LockedStore(const std::string& path) : directory(detail::openDirectory(path)) {
        secure(directory.get(), true);
        lock = FileDescriptor(::openat(directory.get(), ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
        if (lock.get() < 0) detail::ioError("open store lock");
        secure(lock.get(), false);
        while (::flock(lock.get(), LOCK_EX) != 0) { if (errno != EINTR) detail::ioError("lock store"); }
    }
};
bool exists(int directory, const std::string& name) {
    struct stat status{};
    if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) return true;
    if (errno == ENOENT) return false;
    detail::ioError("stat store entry");
}
void sync(int descriptor) {
    while (::fsync(descriptor) != 0) { if (errno != EINTR) detail::ioError("fsync store"); }
}
void noPending(int directory) {
    if (exists(directory, PendingFile)) throw ConfigError("recovery_required", "", "Interrupted transaction; run explicit reconcile before mutations");
}
Catalog readCatalog(int directory, const std::string& name) {
    FileDescriptor file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) detail::ioError("open committed store entry");
    secure(file.get(), false);
    const auto bytes = detail::readDescriptor(file.get());
    auto catalog = ConfigLoader::fromJson(bytes);
    if (catalog->canonicalJson() != bytes) throw ConfigError("store_corrupt", "", "Stored document is not canonical; import through save instead of editing the store");
    return catalog;
}
}
std::string inspectStore(const std::string& path) {
    auto directory = detail::openDirectory(path);
    const auto owner = secureForInspect(directory.get(), true);
    if (exists(directory.get(), PendingFile))
        throw ConfigError("recovery_required", "", "Interrupted transaction; run explicit reconcile before mutations");
    if (!exists(directory.get(), CurrentFile))
        throw ConfigError("missing_configuration", "", "configuration.json is missing");
    detail::FileDescriptor file(::openat(directory.get(), CurrentFile, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) detail::ioError("open committed store entry");
    if (secureForInspect(file.get(), false) != owner)
        throw ConfigError("unsafe_store", "", "Configuration must have the same owner as its store directory");
    const auto bytes = detail::readDescriptor(file.get());
    const auto catalog = ConfigLoader::fromJson(bytes);
    if (catalog->canonicalJson() != bytes)
        throw ConfigError("store_corrupt", "", "Stored document is not canonical; import through save instead of editing the store");
    detail::Json out{{"schema_version", catalog->configuration().schemaVersion},
        {"revision", catalog->configuration().revision},
        {"content_sha256", catalog->contentHash()},
        {"canonical", true},
        {"recovery_required", false},
        {"production_activated", false}};
    return out.dump() + "\n";
}
namespace {
std::string randomName() {
    unsigned char bytes[16];
    std::size_t offset = 0;
    while (offset < sizeof(bytes)) {
        const auto count = ::getrandom(bytes + offset, sizeof(bytes) - offset, 0);
        if (count < 0) { if (errno == EINTR) continue; detail::ioError("temporary filename entropy"); }
        if (count == 0) throw ConfigError("io_error", "", "No temporary filename entropy");
        offset += static_cast<std::size_t>(count);
    }
    const char* hex = "0123456789abcdef";
    std::string name = ".tmp-";
    for (const auto byte : bytes) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    return name;
}
struct Temporary {
    int directory;
    std::string name;
    FileDescriptor file;
    explicit Temporary(int parent) : directory(parent), name(randomName()),
        file(::openat(parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)) {
        if (file.get() < 0) detail::ioError("create temporary file");
    }
    ~Temporary() { if (!name.empty()) ::unlinkat(directory, name.c_str(), 0); }
};
void notify(const StoreFault& hook, StoreStep step) { if (hook) hook(step); }
void writeAll(int file, std::string_view bytes, const StoreFault& hook, StoreStep step) {
    std::size_t offset = 0;
    bool notified = false;
    while (offset < bytes.size()) {
        const auto requested = std::min<std::size_t>(bytes.size() - offset, 512);
        const auto count = ::write(file, bytes.data() + offset, requested);
        if (count < 0) { if (errno == EINTR) continue; detail::ioError("write store file"); }
        if (count == 0) throw ConfigError("io_error", "", "Zero-length store write");
        offset += static_cast<std::size_t>(count);
        if (!notified) { notified = true; notify(hook, step); }
    }
}
void publish(int directory, const std::string& destination, std::string_view bytes,
             const StoreFault& hook, bool backup, bool& published) {
    Temporary temporary(directory);
    writeAll(temporary.file.get(), bytes, hook, backup ? StoreStep::BackupWrite : StoreStep::Write);
    notify(hook, backup ? StoreStep::BackupSync : StoreStep::FileSync);
    sync(temporary.file.get());
    notify(hook, backup ? StoreStep::BackupPublish : StoreStep::Rename);
    if (::renameat(directory, temporary.name.c_str(), directory, destination.c_str()) != 0) detail::ioError("publish store file");
    temporary.name.clear(); published = true;
    notify(hook, backup ? StoreStep::BackupDirectorySync : StoreStep::DirectorySync);
    sync(directory);
}
FileDescriptor backups(int directory) {
    if (::mkdirat(directory, "backups", 0700) != 0 && errno != EEXIST) detail::ioError("create backup directory");
    FileDescriptor result(::openat(directory, "backups", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (result.get() < 0) detail::ioError("open backup directory");
    secure(result.get(), true);
    sync(directory);
    return result;
}
std::string backupName(const Catalog& catalog) {
    return std::to_string(catalog->configuration().revision) + "-" + catalog->contentHash() + ".json";
}
void checkExpected(const Catalog& current, const ExpectedRevision& expected) {
    if (current->configuration().revision != expected.revision || current->contentHash() != expected.hash)
        throw ConfigError("revision_conflict", "", "Expected revision/hash does not match the committed document");
}
std::set<std::string> domainIds(const Configuration& config) {
    std::set<std::string> result{config.system.id};
    for (const auto& item : config.rooms) result.insert(item.id);
    for (const auto& item : config.devices) result.insert(item.id);
    for (const auto& item : config.actions) result.insert(item.id);
    for (const auto& item : config.restrictions) result.insert(item.id);
    for (const auto& item : config.connections) result.insert(item.id);
    return result;
}
std::map<std::string, std::string> identityKinds(const Configuration& config) {
    std::map<std::string, std::string> result{{config.system.id, "system"}};
    for (const auto& item : config.rooms) result[item.id] = "room";
    for (const auto& item : config.devices) result[item.id] = "device";
    for (const auto& item : config.actions) result[item.id] = "action";
    for (const auto& item : config.restrictions) result[item.id] = "restriction";
    for (const auto& item : config.connections) result[item.id] = "connection";
    return result;
}
Catalog nextRevision(const Catalog& candidate, const Catalog& previous,
                     const std::set<std::string>& reactivated = {}) {
    if (!candidate) throw ConfigError("invalid_candidate", "", "Validated candidate required");
    auto value = candidate->configuration();
    if (previous) {
        const auto& old = previous->configuration();
        if (old.revision == MaxRevision) throw ConfigError("revision_limit", "/revision", "Revision exhausted");
        if (old.system.id != value.system.id) throw ConfigError("identity_conflict", "/system/id", "System identity cannot be reassigned");
        const auto oldKinds = identityKinds(old);
        for (const auto& [id, kind] : identityKinds(value)) if (oldKinds.count(id) && oldKinds.at(id) != kind)
            throw ConfigError("identity_conflict", "", "ID cannot be reassigned to a different object category: " + id);
        for (const auto& device : value.devices) if (previous->deviceIndex().count(device.id)) {
            if (old.devices[previous->deviceIndex().at(device.id)].kind != device.kind)
                throw ConfigError("identity_conflict", "/devices", "Device kind cannot change under the same ID");
        }
        for (const auto& action : value.actions) if (previous->actionIndex().count(action.id)) {
            if (old.actions[previous->actionIndex().at(action.id)].operation != action.operation)
                throw ConfigError("identity_conflict", "/actions", "Action operation cannot change under the same ID");
        }
        const auto active = domainIds(value);
        std::set<std::string> retired(old.retiredIds.begin(), old.retiredIds.end());
        retired.insert(value.retiredIds.begin(), value.retiredIds.end());
        for (const auto& id : reactivated) retired.erase(id);
        for (const auto& id : domainIds(old)) if (!active.count(id)) retired.insert(id);
        value.retiredIds.assign(retired.begin(), retired.end());
        value.revision = old.revision + 1;
    } else value.revision = 1;
    return ConfigValidator::validate(std::move(value));
}
std::vector<StoredRevision> listBackups(int directory);
void reserveBackupSlot(int directory, int backupDirectory, const Catalog& previous, const StoreFault& fault);
Catalog commit(int directory, const Catalog& candidate, const Catalog& previous, const StoreFault& fault,
               const std::set<std::string>& reactivated = {}) {
    const auto next = nextRevision(candidate, previous, reactivated);
    if (previous) {
        auto backupDirectory = backups(directory);
        reserveBackupSlot(directory, backupDirectory.get(), previous, fault);
        const auto name = backupName(previous);
        if (exists(backupDirectory.get(), name)) {
            if (readCatalog(backupDirectory.get(), name)->contentHash() != previous->contentHash())
                throw ConfigError("store_corrupt", "", "Backup content mismatch");
            sync(backupDirectory.get());
        } else {
            bool backupPublished = false;
            publish(backupDirectory.get(), name, previous->canonicalJson(), fault, true, backupPublished);
        }
    }
    const detail::Json marker{{"old_hash", previous ? previous->contentHash() : ""},
        {"new_hash", next->contentHash()}, {"old_revision", previous ? previous->configuration().revision : 0},
        {"new_revision", next->configuration().revision}};
    bool markerPublished = false;
    publish(directory, PendingFile, marker.dump() + "\n", {}, false, markerPublished);
    bool published = false;
    try {
        publish(directory, CurrentFile, next->canonicalJson(), fault, false, published);
    } catch (...) {
        if (published) throw ConfigError("durability_indeterminate", "", "New revision is visible; reconcile before further store mutations");
        if (::unlinkat(directory, PendingFile, 0) != 0) throw ConfigError("recovery_required", "", "Failed to clear aborted transaction");
        sync(directory);
        throw;
    }
    if (::unlinkat(directory, PendingFile, 0) != 0) throw ConfigError("recovery_required", "", "Committed revision needs transaction cleanup");
    sync(directory);
    return next;
}
std::vector<StoredRevision> listBackups(int directory) {
    if (!exists(directory, "backups")) return {};
    FileDescriptor backupDirectory(::openat(directory, "backups", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (backupDirectory.get() < 0) detail::ioError("open backups");
    secure(backupDirectory.get(), true);
    const int duplicate = ::fcntl(backupDirectory.get(), F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) detail::ioError("duplicate backup descriptor");
    struct CloseDirectory { void operator()(DIR* entry) const { ::closedir(entry); } };
    std::unique_ptr<DIR, CloseDirectory> entries(::fdopendir(duplicate));
    if (!entries) { ::close(duplicate); detail::ioError("read backup directory"); }
    std::map<std::int64_t, StoredRevision> result;
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(entries.get());
        if (!entry) { if (errno) detail::ioError("list backups"); break; }
        const std::string name(entry->d_name);
        if (name == "." || name == ".." || name.rfind(".tmp-", 0) == 0) continue;
        const auto catalog = readCatalog(backupDirectory.get(), name);
        if (backupName(catalog) != name) throw ConfigError("store_corrupt", "", "Backup name/hash/revision mismatch");
        const auto revision = catalog->configuration().revision;
        struct stat info{};
        if (::fstatat(backupDirectory.get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) detail::ioError("stat backup time");
        if (!result.emplace(revision, StoredRevision{revision, catalog->contentHash(), static_cast<std::int64_t>(info.st_mtime) * 1000}).second)
            throw ConfigError("store_corrupt", "", "Conflicting backups for one revision");
    }
    std::vector<StoredRevision> values;
    for (const auto& [revision, entry] : result) { (void)revision; values.push_back(entry); }
    return values;
}
ExpectedRevision readLastKnownGoodMarker(int directory) {
    FileDescriptor file(::openat(directory, LastKnownGoodFile, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) detail::ioError("open last known good marker");
    secure(file.get(), false);
    const auto marker = detail::parseDocument(detail::readDescriptor(file.get()));
    if (!marker.is_object() || !marker.contains("revision") || !marker.contains("hash") ||
        !marker["revision"].is_number_integer() || !marker["hash"].is_string())
        throw ConfigError("store_corrupt", "", "Malformed last_known_good marker");
    return {marker["revision"].get<std::int64_t>(), marker["hash"].get<std::string>()};
}

void reserveBackupSlot(int directory, int backupDirectory, const Catalog& previous, const StoreFault& fault) {
    // Validate the entire history before any unlink. listBackups orders by revision,
    // never mtime, and rejects untrusted identities, symlinks and conflicting files.
    const auto entries = listBackups(directory);
    bool predecessorPresent = false;
    for (const auto& entry : entries) {
        if (entry.revision > previous->configuration().revision ||
            (entry.revision == previous->configuration().revision && entry.hash != previous->contentHash()))
            throw ConfigError("store_corrupt", "", "Backup revision is not a committed predecessor");
        if (entry.revision == previous->configuration().revision) predecessorPresent = true;
    }
    const auto required = entries.size() + (predecessorPresent ? 0 : 1);
    if (required <= kMaxHistoricalBackups) return;
    const auto removeCount = required - kMaxHistoricalBackups;

    // An older store may have only a marker + ordinary backup. Publish and fsync
    // the existing dedicated snapshot BEFORE deleting that sole recoverable LKG.
    if (exists(directory, LastKnownGoodFile)) {
        const auto marker = readLastKnownGoodMarker(directory);
        for (std::size_t i = 0; i < removeCount; ++i) {
            const auto& entry = entries[i];
            if (entry.revision != marker.revision || entry.hash != marker.hash) continue;
            bool retained = false;
            if (exists(directory, LastKnownGoodSnapshot)) {
                try {
                    const auto snapshot = readCatalog(directory, LastKnownGoodSnapshot);
                    retained = snapshot->configuration().revision == marker.revision && snapshot->contentHash() == marker.hash;
                } catch (const ConfigError& error) {
                    // Malformed/stale JSON can be replaced from the verified backup;
                    // unsafe objects or read failures must not be silently replaced.
                    if (error.code == "unsafe_store" || error.code == "io_error") throw;
                }
            }
            if (retained) {
                // A previous attempt may have published the snapshot but failed
                // its directory fsync. Re-establish durability before unlinking.
                FileDescriptor snapshot(::openat(directory, LastKnownGoodSnapshot, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
                if (snapshot.get() < 0) detail::ioError("open retained LKG snapshot");
                secure(snapshot.get(), false);
                sync(snapshot.get());
                sync(directory);
            } else {
                const auto snapshot = readCatalog(backupDirectory,
                    std::to_string(entry.revision) + "-" + entry.hash + ".json");
                bool published = false;
                publish(directory, LastKnownGoodSnapshot, snapshot->canonicalJson(), fault, true, published);
            }
        }
    }
    for (std::size_t i = 0; i < removeCount; ++i) {
        const auto& entry = entries[i];
        const auto name = std::to_string(entry.revision) + "-" + entry.hash + ".json";
        notify(fault, StoreStep::PruneUnlink);
        if (::unlinkat(backupDirectory, name.c_str(), 0) != 0) detail::ioError("prune historical backup");
    }
    notify(fault, StoreStep::PruneDirectorySync);
    sync(backupDirectory);
    // No pending transaction/current-file write has started yet. Any exception
    // above leaves current unchanged, though some oldest history may be removed.
}

}
ConfigStore::ConfigStore(std::string directory, StoreFault fault) : directory_(std::move(directory)), fault_(std::move(fault)) {}
Catalog ConfigStore::current() const {
    LockedStore store(directory_);
    noPending(store.directory.get());
    return readCatalog(store.directory.get(), CurrentFile);
}
Catalog ConfigStore::initialize(const Catalog& candidate) {
    LockedStore store(directory_); noPending(store.directory.get());
    if (exists(store.directory.get(), CurrentFile)) throw ConfigError("already_initialized", "", "Store already has a committed document");
    return commit(store.directory.get(), candidate, {}, fault_);
}
Catalog ConfigStore::save(const Catalog& candidate, const ExpectedRevision& expected) {
    LockedStore store(directory_); noPending(store.directory.get());
    const auto previous = readCatalog(store.directory.get(), CurrentFile);
    checkExpected(previous, expected);
    return commit(store.directory.get(), candidate, previous, fault_);
}
Catalog ConfigStore::compensateSave(const ExpectedRevision& saved, const ExpectedRevision& predecessor) {
    LockedStore store(directory_); noPending(store.directory.get());
    const auto current = readCatalog(store.directory.get(), CurrentFile);
    checkExpected(current, saved);
    if (predecessor.revision < 1 || predecessor.revision != saved.revision - 1)
        throw ConfigError("invalid_compensation", "", "Only the immediate predecessor of this Save may be restored");
    const auto entries = listBackups(store.directory.get());
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.revision == predecessor.revision && entry.hash == predecessor.hash;
    });
    if (found == entries.end())
        throw ConfigError("invalid_compensation", "", "Exact predecessor backup required");
    auto directory = backups(store.directory.get());
    const auto candidate = readCatalog(directory.get(), std::to_string(found->revision) + "-" + found->hash + ".json");
    // Validated predecessor cannot contain any already retired active IDs.
    // All other retirement history (including removed creations) is retained.
    return commit(store.directory.get(), candidate, current, fault_, domainIds(candidate->configuration()));
}
std::vector<StoredRevision> ConfigStore::revisions() const {
    LockedStore store(directory_); noPending(store.directory.get());
    return listBackups(store.directory.get());
}
Catalog ConfigStore::revision(std::int64_t revision) const {
    LockedStore store(directory_); noPending(store.directory.get());
    const auto current = readCatalog(store.directory.get(), CurrentFile);
    if (current->configuration().revision == revision) return current;
    const auto entries = listBackups(store.directory.get());
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.revision == revision; });
    if (found == entries.end()) throw ConfigError("unknown_revision", "", "No such revision");
    auto directory = backups(store.directory.get());
    return readCatalog(directory.get(), std::to_string(revision) + "-" + found->hash + ".json");
}
Catalog ConfigStore::restoreCandidate(std::int64_t revisionNumber, const ExpectedRevision& expected) const {
    // Read the immutable revision first, then check the captured current identity.
    const auto candidate = revision(revisionNumber);
    LockedStore store(directory_); noPending(store.directory.get());
    const auto current = readCatalog(store.directory.get(), CurrentFile); checkExpected(current, expected);
    return nextRevision(candidate, current);
}
Catalog ConfigStore::restore(std::int64_t revision, const ExpectedRevision& expected) {
    LockedStore store(directory_); noPending(store.directory.get());
    const auto previous = readCatalog(store.directory.get(), CurrentFile); checkExpected(previous, expected);
    const auto entries = listBackups(store.directory.get());
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.revision == revision; });
    if (found == entries.end()) throw ConfigError("unknown_revision", "", "No such backup revision");
    auto backupDirectory = backups(store.directory.get());
    const auto candidate = readCatalog(backupDirectory.get(), std::to_string(revision) + "-" + found->hash + ".json");
    return commit(store.directory.get(), candidate, previous, fault_);
}
Catalog ConfigStore::reconcile() {
    LockedStore store(directory_);
    if (!exists(store.directory.get(), PendingFile)) return readCatalog(store.directory.get(), CurrentFile);
    const auto marker = detail::parseDocument(detail::readAt(store.directory.get(), PendingFile));
    if (!marker.is_object() || marker.size() != 4 || !marker.contains("old_hash") || !marker.contains("new_hash") ||
        !marker.contains("old_revision") || !marker.contains("new_revision") || !marker["old_hash"].is_string() ||
        !marker["new_hash"].is_string() || !marker["old_revision"].is_number_integer() || !marker["new_revision"].is_number_integer())
        throw ConfigError("store_corrupt", "", "Malformed transaction marker");
    Catalog current;
    if (exists(store.directory.get(), CurrentFile)) {
        current = readCatalog(store.directory.get(), CurrentFile);
        if (!((marker["old_hash"] == current->contentHash() && marker["old_revision"] == current->configuration().revision) ||
              (marker["new_hash"] == current->contentHash() && marker["new_revision"] == current->configuration().revision)))
            throw ConfigError("store_corrupt", "", "Committed document matches neither transaction state");
        FileDescriptor file(::openat(store.directory.get(), CurrentFile, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (file.get() < 0) detail::ioError("open reconciliation file");
        sync(file.get());
    } else if (marker["old_hash"] != "" || marker["old_revision"] != 0) {
        throw ConfigError("store_corrupt", "", "Missing committed document during reconciliation");
    }
    sync(store.directory.get());
    if (::unlinkat(store.directory.get(), PendingFile, 0) != 0) detail::ioError("remove reconciled transaction marker");
    sync(store.directory.get());
    return current;
}

void ConfigStore::recordLastKnownGood(const Catalog& applied) {
    if (!applied) throw ConfigError("invalid_request", "", "Cannot record empty Last Known Good");
    LockedStore store(directory_);
    noPending(store.directory.get());
    // Dedicated snapshot — do not write into backups/ (revision history stays save/restore only).
    bool snapshotPublished = false;
    publish(store.directory.get(), LastKnownGoodSnapshot, applied->canonicalJson(), fault_, false, snapshotPublished);
    const auto marker = detail::Json{
        {"revision", applied->configuration().revision},
        {"hash", applied->contentHash()}}.dump();
    bool published = false;
    publish(store.directory.get(), LastKnownGoodFile, marker, fault_, false, published);
}

std::optional<Catalog> ConfigStore::lastKnownGood() const {
    LockedStore store(directory_);
    noPending(store.directory.get());
    if (!exists(store.directory.get(), LastKnownGoodFile)) return std::nullopt;
    const auto marker = readLastKnownGoodMarker(store.directory.get());
    const auto revision = marker.revision;
    const auto& hash = marker.hash;
    if (exists(store.directory.get(), CurrentFile)) {
        try {
            const auto current = readCatalog(store.directory.get(), CurrentFile);
            if (current->configuration().revision == revision && current->contentHash() == hash)
                return current;
        } catch (const ConfigError&) {
        } catch (const std::exception&) {
        }
    }
    if (exists(store.directory.get(), LastKnownGoodSnapshot)) {
        try {
            const auto snapshot = readCatalog(store.directory.get(), LastKnownGoodSnapshot);
            if (snapshot->configuration().revision == revision && snapshot->contentHash() == hash)
                return snapshot;
        } catch (const ConfigError&) {
        } catch (const std::exception&) {
        }
    }
    // Older markers may point only at save() backups.
    const auto entries = listBackups(store.directory.get());
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.revision == revision && entry.hash == hash;
    });
    if (found == entries.end()) return std::nullopt;
    auto directory = backups(store.directory.get());
    return readCatalog(directory.get(), std::to_string(revision) + "-" + hash + ".json");
}
}
