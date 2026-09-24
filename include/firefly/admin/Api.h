#pragma once
#include "firefly/admin/RuntimeWorker.h"
#include "firefly/admin/AdminTranscriber.h"
#include "firefly/config/ConfigStore.h"
#include <memory>
#include <string>
namespace firefly::admin {
struct HttpReply {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};
class Api final {
public:
    Api(config::ConfigStore& store, RuntimeClient& runtime, AdminTranscriber* transcriber = nullptr);
    ~Api();
    HttpReply handle(std::string_view method, std::string_view path, std::string_view body);
    void initializeRuntime();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
