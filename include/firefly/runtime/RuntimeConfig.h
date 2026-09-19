#pragma once
#include "firefly/runtime/DomainView.h"
#include <iosfwd>
namespace firefly::runtime {
// Process-lifetime owner. Loads one complete validated snapshot before models.
class RuntimeConfig final {
public:
    explicit RuntimeConfig(const std::string& path);
    static std::string defaultPath();
    const DomainView& domain() const { return domain_; }
    void writeDiagnostics(std::ostream& output) const;
private:
    const DomainView domain_;
};
}
