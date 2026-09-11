#pragma once
#include <string>
#include <string_view>
namespace firefly {
class LlmEngine {
public:
    virtual ~LlmEngine() = default;
    virtual std::string infer(std::string_view original) = 0;
};
}
