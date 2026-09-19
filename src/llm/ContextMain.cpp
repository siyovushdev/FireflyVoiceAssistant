#include "firefly/llm/LlmContext.h"
#include "firefly/runtime/RuntimeConfig.h"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: firefly-llm-context CONFIG");
        const firefly::runtime::RuntimeConfig owner(argv[1]);
        std::cout << firefly::llm::generateContext(owner.domain()).systemPrompt();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
