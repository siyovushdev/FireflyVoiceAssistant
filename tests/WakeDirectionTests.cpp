#include "firefly/voice/WakeDirectionClient.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    std::string directory;
    int server = -1;
    try {
        if (argc == 2) {
            // Explicit operator fixture: no audio/model/device execution path.
            firefly::voice::WakeDirectionClient client(argv[1]);
            std::cout << "DIRECTION_FIXTURE " << ::getpid() << '\n' << std::flush;
            std::string state;
            while (std::getline(std::cin, state))
                std::cout << "SENT " << client.notify(state) << '\n' << std::flush;
            return 0;
        }
        if (argc != 1) return 2;
        int checks = 0;
        const auto check = [&](bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); };
        char temporary[] = "/tmp/firefly-direction-XXXXXX";
        const auto result = ::mkdtemp(temporary);
        if (!result) throw std::runtime_error("mkdtemp");
        directory = result;
        const auto path = directory + "/control";
        firefly::voice::WakeDirectionClient off;
        check(!off.enabled() && !off.notify("WAKE_DETECTED"), "Default has no direction effect");
        bool rejected = false;
        try { firefly::voice::WakeDirectionClient bad("relative"); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Relative socket rejected");
        ::chmod(directory.c_str(), 0777);
        rejected = false;
        try { firefly::voice::WakeDirectionClient bad(path); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Writable control parent rejected");
        ::chmod(directory.c_str(), 0700);
        firefly::voice::WakeDirectionClient client(path);
        check(!client.notify("WAKE_DETECTED"), "Missing helper is nonfatal and never queued");
        { std::ofstream out(path); out << "ordinary file"; }
        check(!client.notify("WAKE_DETECTED"), "Regular file never receives metadata");
        ::unlink(path.c_str());
        server = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        check(server >= 0, "Server socket");
        int credentials = 1;
        check(::setsockopt(server, SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials)) == 0, "Kernel credentials enabled");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size()+1);
        check(::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Private endpoint bind");
        check(client.notify("WAKE_DETECTED"), "Lifecycle sent without background helper launch");
        char packet[128]{}; char ancillary[CMSG_SPACE(sizeof(ucred))]{};
        iovec data{packet,sizeof(packet)}; msghdr message{};
        message.msg_iov = &data; message.msg_iovlen = 1;
        message.msg_control = ancillary; message.msg_controllen = sizeof(ancillary);
        const auto size = ::recvmsg(server,&message,0);
        check(size > 0 && std::string(packet,static_cast<std::size_t>(size)).find("WAKE_DETECTED ") == 0, "Bounded lifecycle and monotonic timestamp");
        const auto* control = CMSG_FIRSTHDR(&message);
        check(control && control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_CREDENTIALS,
              "Receiver can authenticate actual sender");
        ucred sender{}; std::memcpy(&sender,CMSG_DATA(control),sizeof(sender));
        check(sender.pid == ::getpid() && sender.uid == ::geteuid(), "Kernel PID/UID, not claimed message identity");
        check(!client.notify("включи свет") && !client.notify("WAKE_DETECTED\nPROCESSING"), "No transcripts or arbitrary control commands");
        check(::recv(server,packet,sizeof(packet),0) < 0, "Rejected data not delivered");
        const auto begin = std::chrono::steady_clock::now();
        bool full = false;
        for (int i=0;i<1000;++i) if (!client.notify("WAITING_WAKE")) { full = true; break; }
        check(full && std::chrono::steady_clock::now()-begin < std::chrono::milliseconds(250), "Full helper queue cannot block microphone loop");
        while (::recv(server,packet,sizeof(packet),0) >= 0) {}
        check(client.notify("PROCESSING"), "Next lifecycle succeeds after backpressure");
        const auto count = ::recv(server,packet,sizeof(packet),0);
        check(count > 0 && std::string(packet,static_cast<std::size_t>(count)).find("PROCESSING ") == 0,
              "Dropped lifecycle never replays later");
        check(::recv(server,packet,sizeof(packet),0) < 0, "No hidden retry/background sender");
        ::close(server); server = -1;
        std::filesystem::remove_all(directory);
        std::cout << "PASS " << checks << " bounded direction-channel checks\n";
    } catch (const std::exception& error) {
        if (server >= 0) ::close(server);
        if (!directory.empty()) std::filesystem::remove_all(directory);
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
