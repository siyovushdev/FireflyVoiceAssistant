#include "Support.h"
#include "Ipc.h"
#include <poll.h>
#include <iostream>
int main() {
    admin_test::Model model;
    firefly::admin::RuntimeWorker worker(model);
    try {while(true){pollfd p{3,POLLIN,0};if(::poll(&p,1,-1)<=0 || p.revents&(POLLHUP|POLLERR))break;firefly::admin::sendMessage(3,worker.handle(firefly::admin::receiveMessage(3,5)),5);}}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';}
}
