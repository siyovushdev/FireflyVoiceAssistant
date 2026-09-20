#pragma once
#include "firefly/admin/RuntimeGeneration.h"
#include <string>
namespace firefly::admin {
class RuntimeWorker final {
public:
    explicit RuntimeWorker(ContextModel& model) : model_(model), generations_(model) {}
    std::string handle(std::string_view message);
private:
    ContextModel& model_;
    RuntimeGenerations generations_;
};
class RuntimeClient {
public:
    virtual ~RuntimeClient() = default;
    virtual std::string call(std::string_view message) = 0;
};
}
