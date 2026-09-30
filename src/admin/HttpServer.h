#pragma once
#include "firefly/admin/Api.h"
#include <csignal>
#include <memory>
#include <string>

namespace firefly::admin {
class HttpServer final {
public:
    HttpServer(Api& api, const std::string& bindAddress, unsigned short port);
    ~HttpServer();
    unsigned short port() const;
    void finishClients();
    void run(const volatile std::sig_atomic_t& stopping);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
