#pragma once
#include "firefly/SetupResources.h"
#include <iostream>
#include <memory>

// Compiled only into DemoMain fixtures that link the fake RKLLM API. No runtime
// flag/environment override exists in the production appliance.
inline std::shared_ptr<firefly::SetupResources> fakeSetupResources() {
    std::cerr << "TEST_ONLY synthetic setup memory\n";
    return std::make_shared<firefly::SetupResources>(firefly::SetupResources::Hooks{
        [] { return firefly::SetupResources::Memory{7 * firefly::SetupResources::GiBKiB, 0, 0, 0}; },
        {}, {}, {}});
}
