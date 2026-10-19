#pragma once
#include "firefly/config/Configuration.h"
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace firefly::execution {
// Secrets are never part of the ConfigStore graph or a JSON export.
// Production uses one pre-provisioned private directory, without any runtime
// shell operation or privileged filesystem management.
struct MqttLogin final {
    std::string user, password;
};
class MqttCredentials final {
public:
    static constexpr const char* DefaultDirectory = "/var/lib/firefly/mqtt/credentials";
    explicit MqttCredentials(std::string directory = DefaultDirectory);
    static void validateId(const std::string& id);
    // Throws stable, non-secret error strings on missing, symlink, permission,
    // invalid content, or storage failure. Empty ID means anonymous client.
    MqttLogin read(const std::string& id) const;
    void create(const std::string& id, const std::string& username, const std::string& password);
    void rotate(const std::string& id, const std::string& username, const std::string& password);
    void erase(const std::string& id);
    std::vector<std::string> ids() const;
    void verifyReferences(const config::Catalog& catalog) const;
private:
    std::string directory_;
    mutable std::mutex mutex_;
};
}
