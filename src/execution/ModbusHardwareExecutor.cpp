#include "firefly/execution/ModbusHardwareExecutor.h"
#include "firefly/config/Connectivity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace firefly::execution {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Port {
    int fd = -1; bool serial = false, exclusive = false, restore = false;
    termios original{};
    ~Port() {
        if(fd < 0)return;
        if(restore)::tcsetattr(fd,TCSANOW,&original);
        if(exclusive)::ioctl(fd,TIOCNXCL);
        ::close(fd);
    }
};
void budget(Clock::time_point end) { if(Clock::now()>=end)throw Failure("modbus_timeout"); }
void waitFd(int fd, short events, Clock::time_point end) {
    for(;;) {
        budget(end);
        auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(end-Clock::now()).count();
        if(ns<=0)throw Failure("modbus_timeout");
        timespec timeout{static_cast<time_t>(ns/1000000000),static_cast<long>(ns%1000000000)};
        pollfd p{fd,events,0};const int result=::ppoll(&p,1,&timeout,nullptr);
        if(result<0 && errno==EINTR)continue;
        if(result==0)throw Failure("modbus_timeout");
        if(result<0)throw Failure("modbus_io_error");
        if(p.revents&events) {budget(end);return;}
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL))throw Failure("modbus_disconnected");
    }
}
void quietLine(int fd, Clock::time_point end, std::chrono::microseconds interval) {
    budget(end);const auto until=Clock::now()+interval;
    if(until>end)throw Failure("modbus_timeout");
    for(;;) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(until-Clock::now()).count();
        if(ns<=0)break;
        timespec t{static_cast<time_t>(ns/1000000000),static_cast<long>(ns%1000000000)};
        pollfd p{fd,POLLIN,0};const auto n=::ppoll(&p,1,&t,nullptr);
        if(n<0 && errno==EINTR)continue;
        if(n<0)throw Failure("modbus_io_error");
        if(n>0)throw Failure("modbus_response_mismatch");
    }
    budget(end);
}
std::shared_ptr<std::timed_mutex> busLock(const config::Connection& c) {
    static std::mutex guard;
    static std::map<std::string,std::weak_ptr<std::timed_mutex>> locks;
    const auto key=c.type==config::ExecutorKind::ModbusRtu ? c.serial.port : c.network.host+":"+std::to_string(c.network.port);
    std::lock_guard<std::mutex> lock(guard);
    for(auto i=locks.begin();i!=locks.end();) {if(i->second.expired())i=locks.erase(i);else ++i;}
    if(auto existing=locks[key].lock())return existing;
    auto result=std::make_shared<std::timed_mutex>();locks[key]=result;return result;
}
std::uint16_t crc(const Bytes& data) {
    std::uint16_t value=0xffff;
    for(auto b:data) {value^=b;for(int i=0;i<8;++i)value=(value&1)?(value>>1)^0xa001:value>>1;}
    return value;
}
void word(Bytes& data, std::uint16_t n) {data.push_back(n>>8);data.push_back(n&255);}
std::uint16_t wordAt(const Bytes& data, std::size_t pos) {return (std::uint16_t(data.at(pos))<<8)|data.at(pos+1);}
speed_t speed(std::int64_t rate) {
    switch(rate) {
        case 1200:return B1200;case 2400:return B2400;case 4800:return B4800;
        case 9600:return B9600;case 19200:return B19200;case 38400:return B38400;
        case 57600:return B57600;case 115200:return B115200;case 230400:return B230400;
        default:throw Failure("modbus_serial_parameters");
    }
}
void openPort(Port& p,const config::Connection& c,Clock::time_point end) {
    budget(end);
    if(c.type==config::ExecutorKind::ModbusTcp) {
        sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_port=htons(c.network.port);
        if(::inet_pton(AF_INET,c.network.host.c_str(),&peer.sin_addr)!=1)throw Failure("modbus_invalid_host");
        p.fd=::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(p.fd<0)throw Failure("modbus_connection_failure");
        const int result=::connect(p.fd,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
        if(result<0 && errno!=EINPROGRESS)throw Failure("modbus_connection_failure");
        if(result<0) {
            waitFd(p.fd,POLLOUT,end);int error=0;socklen_t len=sizeof(error);
            if(::getsockopt(p.fd,SOL_SOCKET,SO_ERROR,&error,&len)<0 || error)throw Failure("modbus_connection_failure");
        }
    } else {
        p.serial=true;
        p.fd=::open(c.serial.port.c_str(),O_RDWR|O_NONBLOCK|O_NOCTTY|O_CLOEXEC|O_NOFOLLOW);
        if(p.fd<0)throw Failure(errno==EBUSY?"modbus_port_busy":"modbus_port_unavailable");
        struct stat info{};
        if(::fstat(p.fd,&info)!=0 || !S_ISCHR(info.st_mode) || !::isatty(p.fd))throw Failure("modbus_unsafe_port");
        if(::flock(p.fd,LOCK_EX|LOCK_NB)!=0)throw Failure("modbus_port_busy");
        if(::ioctl(p.fd,TIOCEXCL)!=0)throw Failure("modbus_port_busy");
        p.exclusive=true;
        if(::tcgetattr(p.fd,&p.original)!=0)throw Failure("modbus_serial_parameters");
        p.restore=true;
        auto t=p.original;::cfmakeraw(&t);
        t.c_cflag&=~(CSIZE|PARENB|PARODD|CSTOPB|CRTSCTS);t.c_cflag|=CS8|CLOCAL|CREAD;
        if(c.serial.parity!="none")t.c_cflag|=PARENB;
        if(c.serial.parity=="odd")t.c_cflag|=PARODD;
        if(c.serial.stopBits==2)t.c_cflag|=CSTOPB;
        t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;
        const auto baud=speed(c.serial.baudRate);
        if(::cfsetispeed(&t,baud)!=0 || ::cfsetospeed(&t,baud)!=0 || ::tcsetattr(p.fd,TCSANOW,&t)!=0)throw Failure("modbus_serial_parameters");
        if(::tcflush(p.fd,TCIFLUSH)!=0)throw Failure("modbus_io_error");
    }
    budget(end);
}
void sendAll(Port& p,const Bytes& bytes,Clock::time_point end) {
    std::size_t sent=0;
    while(sent<bytes.size()) {
        waitFd(p.fd,POLLOUT,end);budget(end);
        const auto n=p.serial?::write(p.fd,bytes.data()+sent,bytes.size()-sent):
            ::send(p.fd,bytes.data()+sent,bytes.size()-sent,MSG_NOSIGNAL);
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))continue;
        if(n<=0)throw Failure("modbus_disconnected");
        sent+=static_cast<std::size_t>(n);
    }
}
void receive(Port& p,Bytes& bytes,std::size_t count,Clock::time_point end,
             std::chrono::microseconds gap=std::chrono::microseconds::zero()) {
    while(bytes.size()<count) {
        const auto next=gap.count()>0 && !bytes.empty()?std::min(end,Clock::now()+gap):end;
        waitFd(p.fd,POLLIN,next);std::array<unsigned char,260> chunk{};
        const auto n=::read(p.fd,chunk.data(),std::min(chunk.size(),count-bytes.size()));
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))continue;
        if(n<=0)throw Failure("modbus_disconnected");
        bytes.insert(bytes.end(),chunk.begin(),chunk.begin()+n);
    }
    budget(end);
}
struct Exchange { HardwareResult result; Bytes pdu; };
Exchange exchange(const config::Connection& c,const Bytes& request,bool reading,std::int64_t cap) {
    const auto timeout=cap>0?std::min(c.timeoutMs,cap):c.timeoutMs;
    const auto end=Clock::now()+std::chrono::milliseconds(timeout);
    try {
        const auto mutex=busLock(c);std::unique_lock<std::timed_mutex> lock(*mutex,std::defer_lock);
        if(!lock.try_lock_until(end))throw Failure("modbus_busy");
        Port port;openPort(port,c,end);Bytes reply;
        if(!port.serial) {
            static std::atomic<unsigned> sequence{1};const auto id=static_cast<std::uint16_t>(sequence++);
            Bytes frame;word(frame,id);word(frame,0);word(frame,static_cast<std::uint16_t>(request.size()+1));
            frame.push_back(static_cast<unsigned char>(c.modbus.unitId));frame.insert(frame.end(),request.begin(),request.end());
            sendAll(port,frame,end);Bytes header;receive(port,header,7,end);
            const auto length=wordAt(header,4);
            if(wordAt(header,0)!=id || wordAt(header,2)!=0 || header[6]!=c.modbus.unitId || length<3 || length>254)
                throw Failure("modbus_response_mismatch");
            receive(port,reply,length-1,end);
        } else {
            const int bits=1+8+(c.serial.parity=="none"?0:1)+static_cast<int>(c.serial.stopBits);
            const auto idle=std::chrono::microseconds(c.serial.baudRate>19200?1750:(3500000LL*bits+c.serial.baudRate-1)/c.serial.baudRate);
            const auto gap=std::chrono::microseconds(c.serial.baudRate>19200?750:(1500000LL*bits+c.serial.baudRate-1)/c.serial.baudRate);
            quietLine(port.fd,end,idle);
            Bytes frame{static_cast<unsigned char>(c.modbus.unitId)};frame.insert(frame.end(),request.begin(),request.end());
            const auto sum=crc(frame);frame.push_back(sum&255);frame.push_back(sum>>8);
            sendAll(port,frame,end);Bytes response;receive(port,response,1,end);receive(port,response,2,end,gap);
            const bool exception=response[1]==(request[0]|0x80);
            std::size_t expected=exception?5:8;
            if(!exception && reading) {
                receive(port,response,3,end,gap);
                const auto size=request[0]<=2?1:2;
                if(response[2]!=size)throw Failure("modbus_response_mismatch");
                expected=static_cast<std::size_t>(size)+5;
            }
            receive(port,response,expected,end,gap);
            const auto wire=static_cast<std::uint16_t>(response[expected-2]|(response[expected-1]<<8));
            response.resize(expected-2);
            if(crc(response)!=wire)throw Failure("modbus_crc_error");
            if(response[0]!=c.modbus.unitId)throw Failure("modbus_response_mismatch");
            reply.assign(response.begin()+1,response.end());
            // Preserve inter-frame silence while owning the shared serial bus.
            quietLine(port.fd,end,idle);
        }
        if(reply.size()==2 && reply[0]==(request[0]|0x80))
            return {{false,"modbus_exception",std::to_string(reply[1]),ReachabilityEvidence::Reachable},{}};
        if(reply.empty() || reply[0]!=request[0])throw Failure("modbus_response_mismatch");
        if(reading) {
            const std::size_t size=request[0]<=2?1:2;
            if(reply.size()!=size+2 || reply[1]!=size || (request[0]<=2 && (reply[2]&0xfe)))throw Failure("modbus_response_mismatch");
        } else if(reply!=request)throw Failure("modbus_response_mismatch");
        return {{true,{},{},ReachabilityEvidence::Reachable},std::move(reply)};
    } catch(const Failure& e) {
        const std::string error=e.what();
        const bool down=error=="modbus_timeout" || error=="modbus_disconnected" || error=="modbus_connection_failure" || error=="modbus_port_unavailable";
        return {{false,error,{},down?ReachabilityEvidence::Unreachable:ReachabilityEvidence::None},{}};
    }
}
}
HardwareResult ModbusHardwareExecutor::execute(const config::Binding& binding,const actions::GenericAction& action) {
    try {
        if(!catalog_ || binding.targetId!=action.targetId() || binding.actionId!=action.actionId() || !catalog_->actionIndex().count(binding.actionId))
            return {false,"modbus_invalid_mapping",{},ReachabilityEvidence::None};
        const auto* c=config::findConnection(catalog_->configuration(),binding.connectionId);
        config::validateModbusBinding(binding.modbus,c,binding.executor,catalog_->configuration().actions.at(catalog_->actionIndex().at(binding.actionId)));
        const auto& m=binding.modbus;
        const auto& value=m.value?*m.value:action.arguments().at(*m.argument);
        std::uint16_t wire=0;
        if(m.function==5) {
            if(!std::holds_alternative<bool>(value.data()))throw std::invalid_argument("boolean required");
            wire=std::get<bool>(value.data())?0xff00:0;
        } else {
            const auto n=value.value();const auto low=m.dataType=="int16"?-32768:0,high=m.dataType=="int16"?32767:65535;
            if(n<low || n>high)throw std::invalid_argument("range");
            wire=static_cast<std::uint16_t>(n);
        }
        Bytes request{static_cast<unsigned char>(m.function)};word(request,m.address);word(request,wire);
        return exchange(*c,request,false,0).result;
    } catch(const std::exception&) {return {false,"modbus_invalid_mapping",{},ReachabilityEvidence::None};}
}
HardwareObservation ModbusHardwareExecutor::observe(const config::ObservationBinding& binding) {return observe(binding,0);}
HardwareObservation ModbusHardwareExecutor::observe(const config::ObservationBinding& binding,std::int64_t timeoutLimitMs) {
    try {
        if(!catalog_ || !catalog_->deviceIndex().count(binding.targetId))throw std::invalid_argument("target");
        const auto* c=config::findConnection(catalog_->configuration(),binding.connectionId);
        config::validateModbusObservation(binding.modbus,c,binding.executor,catalog_->configuration().devices.at(catalog_->deviceIndex().at(binding.targetId)));
        const auto& m=binding.modbus;Bytes request{static_cast<unsigned char>(m.function)};word(request,m.address);word(request,1);
        auto reply=exchange(*c,request,true,timeoutLimitMs);
        if(!reply.result.success)return {reply.result,{}};
        if(m.function<=2)return {reply.result,{{m.field,actions::Scalar(bool(reply.pdu.at(2)&1))}}};
        const auto raw=wordAt(reply.pdu,2);
        const std::int64_t value=m.dataType=="int16" && raw>=32768?std::int64_t(raw)-65536:raw;
        return {reply.result,{{m.field,actions::Scalar(value)}}};
    } catch(const std::exception&) {return {{false,"modbus_invalid_mapping",{},ReachabilityEvidence::None},{}};}
}
}
