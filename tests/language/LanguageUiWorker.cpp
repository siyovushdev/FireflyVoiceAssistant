#include "../admin/Support.h"
#include "Ipc.h"
#include <poll.h>
struct Model:admin_test::Model {
    bool admin = false;
    bool setupRequired() const override { return true; }
    void enterSetup() override { admin = true; }
    void leaveSetup(const std::string& text) override { admin = false; useContext(text); }
    void useAdminContext(const std::string& text, int) override {
        if (!admin) throw std::runtime_error("setup_required");
        prompt = text;
    }
    std::string inferAdmin(std::string_view text) override {
        if (!admin) throw std::runtime_error("setup_required");
        return infer(text);
    }
    std::string infer(std::string_view) override {
        ++inferences;
        return R"({"aliases":["включи","включить","зажги"]})";
    }
};
int main(){Model model;firefly::admin::RuntimeWorker worker(model);
    try{while(true){pollfd p{3,POLLIN,0};if(::poll(&p,1,-1)<=0||p.revents&(POLLHUP|POLLERR))break;
        firefly::admin::sendMessage(3,worker.handle(firefly::admin::receiveMessage(3,5)),5);}}
    catch(...){return 1;}
}
