#include "firefly/admin/NetworkPolicy.h"
#include <charconv>
#include <stdexcept>

namespace firefly::admin {
void validateAdminBind(std::string_view address) {
    if (address != "127.0.0.1")
        throw std::invalid_argument("V1 Admin bind must be exactly 127.0.0.1; use an SSH/Tailscale TCP tunnel for remote access.");
}

bool AdminAuthority::allowed() const {
    return host == "127.0.0.1" || host == "localhost";
}

AdminAuthority parseAdminAuthority(std::string_view value) {
    const auto invalid = [] { throw std::invalid_argument("Malformed Host authority."); };
    AdminAuthority result;
    const auto colon = value.find(':');
    const auto hostname = value.substr(0, colon);
    if (hostname.empty() || hostname.size() > 253) invalid();
    // DNS-style ASCII labels only. Even valid foreign names are never resolved.
    std::size_t labelLength = 0;
    char previous = 0;
    for (char c : hostname) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '.') {
            if (labelLength == 0 || previous == '-') invalid();
            labelLength = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) invalid();
            if (labelLength == 0 && c == '-') invalid();
            if (++labelLength > 63) invalid();
        }
        result.host += c;
        previous = c;
    }
    if (previous == '-') invalid();
    // A trailing DNS dot is syntactically valid but is not an allowed alias.
    if (colon != std::string_view::npos) {
        const auto port = value.substr(colon + 1);
        if (port.empty()) invalid();
        for (const char c : port) if (c < '0' || c > '9') invalid();
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), result.port);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
            result.port == 0 || result.port > 65535) invalid();
    }
    return result;
}

bool isSameAdminOrigin(std::string_view origin, const AdminAuthority& host) {
    if (origin.substr(0, 7) != "http://") return false;
    try {
        const auto authority = parseAdminAuthority(origin.substr(7));
        return authority.allowed() && authority.host == host.host && authority.port == host.port;
    } catch (const std::invalid_argument&) {
        return false;
    }
}
}
