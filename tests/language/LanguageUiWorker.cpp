#include "../admin/Support.h"
#include "Ipc.h"
#include <poll.h>
struct Model:admin_test::Model {
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
