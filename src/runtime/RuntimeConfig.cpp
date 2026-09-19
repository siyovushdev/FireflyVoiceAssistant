#include "firefly/runtime/RuntimeConfig.h"
#include "firefly/config/ConfigLoader.h"
#include "RuntimeConfigDefaults.h"
#include <ostream>

namespace firefly::runtime {
namespace {
config::Catalog loadSnapshot(const std::string& path) {
    try {
        return config::ConfigLoader::fromFile(path);
    } catch (const config::ConfigError& error) {
        throw config::ConfigError(error.code, error.path,
            "Configuration startup failed [" + error.code + " " + error.path + "]: " + error.what());
    }
}
}
RuntimeConfig::RuntimeConfig(const std::string& path) : domain_(loadSnapshot(path)) {}
std::string RuntimeConfig::defaultPath() { return DefaultConfigPath; }
void RuntimeConfig::writeDiagnostics(std::ostream& output) const {
    const auto& snapshot = domain_.snapshot();
    output << "CONFIG_RUNTIME revision=" << snapshot->configuration().revision
           << " sha256=" << snapshot->contentHash()
           << " action_model=generic domain_authority=config\n";
}
}
