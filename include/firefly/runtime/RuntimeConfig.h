#pragma once
#include "firefly/runtime/ShadowComparison.h"
#include <iosfwd>

namespace firefly::runtime {
// Process-lifetime owner. Construction loads exactly one file or throws before
// command readiness. No reload, setters, store writes, or command dependencies.
class RuntimeConfig final {
public:
    explicit RuntimeConfig(const std::string& path);
    static std::string defaultPath();
    const DomainView& domain() const { return domain_; }
    const ShadowComparison& comparison() const { return comparison_; }
    const ShadowComparison& brightnessComparison() const { return brightnessComparison_; }
    const ShadowComparison& entranceComparison() const { return entranceComparison_; }
    void writeDiagnostics(std::ostream& output) const;
private:
    const DomainView domain_;
    const ShadowComparison comparison_;
    const ShadowComparison brightnessComparison_;
    const ShadowComparison entranceComparison_;
};
}
