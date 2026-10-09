#pragma once
#include <string>
#include <string_view>

namespace firefly::admin {
// V1 has no authentication: validate before store/runtime initialization.
void validateAdminBind(std::string_view address);

struct AdminAuthority {
    std::string host;
    unsigned port = 80;
    bool allowed() const;
};
// Strict ASCII hostname[:decimal-port], no URL syntax. Throws invalid_argument.
AdminAuthority parseAdminAuthority(std::string_view value);
bool isSameAdminOrigin(std::string_view origin, const AdminAuthority& host);
}
