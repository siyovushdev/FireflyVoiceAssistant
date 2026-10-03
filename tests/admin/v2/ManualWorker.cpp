// Test-only failure seam: reject Apply after review/Save, leave real worker active.
#include "../Support.h"
#include "Ipc.h"
#include <cstdlib>
#include <poll.h>
#include <iostream>
class Model final : public admin_test::Model {
public:
    void enterSetup() override {}
};
int main() {
    Model model;
    firefly::admin::RuntimeWorker worker(model);
    try {
        while (true) {
            pollfd descriptor{3, POLLIN, 0};
            if (::poll(&descriptor, 1, -1) <= 0 || descriptor.revents & (POLLHUP | POLLERR)) break;
            const auto bytes = firefly::admin::receiveMessage(3, 5);
            const auto request = admin_test::Json::parse(bytes);
            const auto* marker = std::getenv("FIREFLY_B2_FAIL_APPLY_FILE");
            std::string reply;
            if (marker && std::filesystem::exists(marker) && request.value("method", "") == "apply") {
                auto response = admin_test::Json::parse(worker.handle(R"({"method":"observe"})"));
                response["ok"] = false;
                response["errors"] = {{{"code", "fixture_apply_failure"}, {"path", ""}, {"message", "Test fixture: context admission failed after Save"}}};
                reply = response.dump();
            } else reply = worker.handle(bytes);
            firefly::admin::sendMessage(3, reply, 5);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
}
