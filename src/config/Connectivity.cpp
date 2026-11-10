#include "firefly/config/Connectivity.h"
#include "firefly/actions/ScalarJson.h"
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>

namespace firefly::config {
namespace {
[[noreturn]] void invalid(const std::string& message) {
    throw ConfigError("malformed_binding", "/connections", message);
}
bool clean(const std::string& text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
void path(const std::string& value, bool base = false) {
    if (base && value.empty()) return;
    if (value.empty() || value.front() != '/' || value.rfind("//", 0) == 0 || !clean(value) ||
        value.find_first_of("#\\{}$") != std::string::npos || value.size() > 512 ||
        (base && value.find('?') != std::string::npos)) invalid("Path must be a literal absolute path without fragments or templates");
}
void url(const std::string& value, bool base) {
    if (value.rfind("http://", 0) != 0 || !clean(value) || value.size() > 1024 ||
        value.find_first_of("@#\\{}$") != std::string::npos) invalid("Only http://numeric-IPv4[:port] is supported; credentials, TLS and templates are not supported");
    const auto slash = value.find('/', 7);
    const auto authority = value.substr(7, slash == std::string::npos ? slash : slash - 7);
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    in_addr address{};
    if (::inet_pton(AF_INET, host.c_str(), &address) != 1) invalid("Numeric IPv4 required; DNS is not supported");
    if (colon != std::string::npos) {
        const auto port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
            std::stoi(port) < 1 || std::stoi(port) > 65535) invalid("Invalid TCP port");
    }
    if (slash != std::string::npos) {
        if (base && value.substr(slash) != "/") invalid("Use base_path separately from base_url");
        path(value.substr(slash));
    }
}
}
std::string executorName(ExecutorKind kind) {
    switch (kind) {
        case ExecutorKind::Http: return "http";
        case ExecutorKind::Mqtt: return "mqtt";
        case ExecutorKind::ModbusTcp: return "modbus_tcp";
        case ExecutorKind::ModbusRtu: return "modbus_rtu";
        case ExecutorKind::Tcp: return "tcp";
        case ExecutorKind::Udp: return "udp";
        case ExecutorKind::Serial: return "serial";
        case ExecutorKind::Can: return "can";
        default: throw ConfigError("unsupported_executor", "/connections/type", "Unknown transport");
    }
}
ExecutorKind executorKind(const std::string& name) {
    for (auto kind : {ExecutorKind::Http, ExecutorKind::Mqtt, ExecutorKind::ModbusTcp, ExecutorKind::ModbusRtu, ExecutorKind::Tcp, ExecutorKind::Udp, ExecutorKind::Serial, ExecutorKind::Can})
        if (executorName(kind) == name) return kind;
    return ExecutorKind::Unknown;
}
const Connection* findConnection(const Configuration& config, const std::string& id) {
    for (const auto& connection : config.connections) if (connection.id == id) return &connection;
    return nullptr;
}
void validateConnections(const Configuration& config) {
    if (config.schemaVersion != 2 && (!config.connections.empty() || !config.observations.empty()))
        throw ConfigError("unsupported_version", "/connections", "Connections require v2");
    for (std::size_t i = 0; i < config.connections.size(); ++i) {
        const auto& c = config.connections[i];
        std::string field;
        try {
            field = "/timeout_ms";
            if (c.timeoutMs < 50 || c.timeoutMs > 10000) invalid("Timeout must be 50..10000 ms");
            if (c.type == ExecutorKind::Http) { field = "/http/base_url"; url(c.http.baseUrl, true); field = "/http/base_path"; path(c.http.basePath, true); }
            else if(isModbus(c.type)) {
                field="/modbus/unit_id";
                if(c.modbus.unitId<1 || c.modbus.unitId>247)invalid("Modbus unit must be 1..247; broadcast is not supported");
                if(c.type==ExecutorKind::ModbusTcp) {
                    field="/network"; in_addr address{};
                    if(::inet_pton(AF_INET,c.network.host.c_str(),&address)!=1 || c.network.port<1 || c.network.port>65535)
                        invalid("Modbus TCP requires numeric IPv4 and port 1..65535");
                } else {
                    field="/serial";
                    bool literal=false;
                    for(const auto* prefix:{"/dev/ttyUSB","/dev/ttyACM","/dev/ttyS","/dev/ttyAMA","/dev/pts/"}) {
                        const std::string stem(prefix);
                        if(c.serial.port.rfind(stem,0)==0) {
                            const auto suffix=c.serial.port.substr(stem.size());
                            literal=!suffix.empty() && suffix.size()<=8 && suffix.find_first_not_of("0123456789")==std::string::npos;
                            if(literal)break;
                        }
                    }
                    if(!literal)invalid("Modbus RTU requires a literal ttyUSB/ttyACM/ttyS/ttyAMA port, no symlinks or traversal");
                    const std::set<std::int64_t> rates={1200,2400,4800,9600,19200,38400,57600,115200,230400};
                    if(!rates.count(c.serial.baudRate) || (c.serial.parity!="none" && c.serial.parity!="even" && c.serial.parity!="odd") ||
                        (c.serial.stopBits!=1 && c.serial.stopBits!=2))invalid("Unsupported Modbus serial parameters");
                }
            } else {
                field = "/type";
                if (c.type != ExecutorKind::Mqtt && c.enabled) throw ConfigError("unsupported_executor", "/connections", "Planned transports must remain disabled");
                if (c.type == ExecutorKind::Mqtt || c.type == ExecutorKind::Tcp || c.type == ExecutorKind::Udp) {
                    field = "/network";
                    // Historical *disabled* MQTT/TCP/UDP connection templates
                    // were allowed to store arbitrary non-whitespace hostnames.
                    // Tightening their validation would corrupt old ConfigStores.
                    if (c.network.host.empty() || !clean(c.network.host) ||
                        c.network.port < 1 || c.network.port > 65535) invalid("Invalid network host/port");
                    if (c.type == ExecutorKind::Mqtt && c.enabled &&
                        (c.network.host.size() > 253 ||
                         c.network.host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos ||
                         c.network.host.front() == '-' || c.network.host.back() == '-' ||
                         c.network.host.front() == '.' || c.network.host.back() == '.' ||
                         c.network.host.find("..") != std::string::npos))
                        invalid("Enabled MQTT requires a literal IPv4/DNS hostname");
                    if (c.type == ExecutorKind::Mqtt) {
                        field = "/mqtt/credentials_id";
                        if (!c.mqtt.credentialsId.empty() && (c.mqtt.credentialsId.size() > 64 ||
                            c.mqtt.credentialsId.front() < 'a' || c.mqtt.credentialsId.front() > 'z' ||
                            c.mqtt.credentialsId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos))
                            invalid("Credentials reference must be a stable identifier, never a password");
                    }
                } else if (c.type == ExecutorKind::Serial) {
                    if (c.serial.port.rfind("/dev/", 0) != 0 || !clean(c.serial.port) || c.serial.baudRate < 50 || c.serial.baudRate > 4000000) invalid("Serial port and baud rate required");
                } else if (c.type == ExecutorKind::Can) {
                    if (c.can.interface.empty() || !clean(c.can.interface)) invalid("CAN interface required");
                } else invalid("Unknown transport type");
            }
        } catch (const ConfigError& error) {
            throw ConfigError(error.code, "/connections/" + std::to_string(i) + field, error.what());
        }
    }
}
HttpBinding effectiveHttp(const HttpBinding& mapping, const Connection* connection) {
    auto result = mapping;
    if (connection) {
        auto base = connection->http.baseUrl;
        if (!base.empty() && base.back() == '/') base.pop_back();
        auto prefix = connection->http.basePath;
        if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        result.url = base + prefix + mapping.path;
        if (result.timeoutMs == 0) result.timeoutMs = connection->timeoutMs;
    }
    return result;
}
void validateHttpMapping(const HttpBinding& h, const Connection* c, bool observe) {
    if (c) {
        if (c->type != ExecutorKind::Http) invalid("Mapping transport differs from connection type");
        if (!h.url.empty()) invalid("Connection bindings use path, never url");
        path(h.path);
    } else {
        if (!h.path.empty()) invalid("A path requires connection_id");
        url(h.url, false);
    }
    if (observe) {
        if (h.method != "GET" || !h.body.empty() || h.argument) invalid("Observe requires GET without body or arguments");
    } else if (h.method != "POST" && h.method != "PUT" && h.method != "PATCH" && h.method != "DELETE" && h.method != "GET") invalid("Unsupported HTTP method");
    if (h.method == "GET" && (!h.body.empty() || h.argument)) invalid("GET must not carry a body");
    const auto effective = effectiveHttp(h, c);
    url(effective.url, false);
    if (effective.timeoutMs < 50 || effective.timeoutMs > 10000) invalid("Timeout must be 50..10000 ms (0 inherits Connection)");
    if (h.expectedStatus != 0 && (h.expectedStatus < 200 || h.expectedStatus > 299)) invalid("Success status must be 2xx");
    if (h.contentType.empty() || h.contentType.size() > 128 || std::any_of(h.contentType.begin(), h.contentType.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) invalid("Invalid Content-Type");
    std::set<std::string> names;
    if (h.headers.size() > 16) invalid("Too many headers");
    for (const auto& [name, value] : h.headers) {
        std::string lower;
        for (unsigned char ch : name) lower += static_cast<char>(std::tolower(ch));
        if (name.empty() || name.size() > 64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos ||
            !names.insert(lower).second || (lower != "accept" && lower.rfind("x-", 0) != 0) ||
            lower.find("auth") != std::string::npos || lower.find("key") != std::string::npos || lower.find("token") != std::string::npos ||
            value.size() > 512 || std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) invalid("Only non-secret Accept / X-* headers are supported");
    }
    nlohmann::json body = nlohmann::json::object();
    for (const auto& [key, value] : h.body) {
        body[key] = actions::scalarJson(value);
        if (key.empty() || key.size() > 64) invalid("Body keys must be 1..64 bytes");
        if (const auto* text = std::get_if<std::string>(&value.data()))
            if (text->find("${") != std::string::npos || text->find("{{") != std::string::npos) invalid("Templates are not supported; use declared argument");
    }
    if (body.dump().size() > 16384) invalid("Request body exceeds 16 KiB");
}
}

namespace firefly::config {
namespace {
void mqttTopic(const std::string& topic) {
    if (topic.empty() || topic.size() > 256 || topic.find_first_of("+#") != std::string::npos ||
        std::any_of(topic.begin(), topic.end(), [](unsigned char ch) { return ch == 0 || ch < 32 || ch == 127; }))
        throw ConfigError("malformed_binding", "/mqtt/topic", "MQTT topic must be a literal non-wildcard topic");
}
}
void validateMqttBinding(const MqttBinding& binding, const Connection* connection) {
    if (!connection || connection->type != ExecutorKind::Mqtt || !connection->enabled)
        throw ConfigError("malformed_binding", "/mqtt", "MQTT mapping requires an enabled MQTT connection");
    mqttTopic(binding.topic);
    if (binding.qos < 0 || binding.qos > 1 || binding.payload.size() > 4096 ||
        binding.payload.find("${") != std::string::npos || binding.payload.find("{{") != std::string::npos ||
        binding.payload.find('\0') != std::string::npos)
        throw ConfigError("malformed_binding", "/mqtt", "Invalid MQTT payload, QoS or template");
}
void validateMqttObservation(const MqttObservation& observation, const Connection* connection) {
    if (!connection || connection->type != ExecutorKind::Mqtt || !connection->enabled)
        throw ConfigError("malformed_binding", "/mqtt", "MQTT observe requires an enabled MQTT connection");
    mqttTopic(observation.topic);
    if (observation.qos < 0 || observation.qos > 1 || (observation.format != "text" && observation.format != "json"))
        throw ConfigError("malformed_binding", "/mqtt", "Invalid MQTT observation format or QoS");
    if (observation.format == "text" &&
        (observation.field.empty() || observation.truePayload.empty() || observation.falsePayload.empty() ||
         observation.truePayload == observation.falsePayload || observation.truePayload.size() > 128 ||
         observation.falsePayload.size() > 128))
        throw ConfigError("malformed_binding", "/mqtt", "Text observation requires distinct ON/OFF values and a target field");
}
}

namespace firefly::config {
namespace {
void modbusMapping(const Connection* c, ExecutorKind kind, std::int64_t address) {
    if(!c || !isModbus(kind) || c->type!=kind || !c->enabled)
        throw ConfigError("malformed_binding","/modbus","Mapping requires an enabled matching Modbus connection");
    if(address<0 || address>65535)throw ConfigError("malformed_binding","/modbus/address","Use the zero-based protocol address 0..65535");
}
void modbusType(const std::string& type, bool coil) {
    if(coil ? type!="boolean" : (type!="uint16" && type!="int16"))
        throw ConfigError("malformed_binding","/modbus/data_type","Coils require boolean; registers require uint16 or int16");
}
}
void validateModbusBinding(const ModbusBinding& m, const Connection* c, ExecutorKind kind, const ActionDefinition& action) {
    modbusMapping(c,kind,m.address);
    if(m.function!=5 && m.function!=6)throw ConfigError("unsupported_executor","/modbus/function","Only write-single-coil (5) and write-single-register (6) are supported");
    modbusType(m.dataType,m.function==5);
    if(m.value.has_value()==m.argument.has_value())throw ConfigError("malformed_binding","/modbus","Specify exactly one fixed value or declared argument");
    const auto low=m.dataType=="int16"?-32768:0,high=m.dataType=="int16"?32767:65535;
    if(m.value) {
        if(!action.parameters.empty())throw ConfigError("malformed_binding","/modbus/value","Parameterized actions require an argument mapping");
        if(m.function==5) {
            if(!std::holds_alternative<bool>(m.value->data()))throw ConfigError("malformed_binding","/modbus/value","Coil write requires boolean");
        } else if(!std::holds_alternative<std::int64_t>(m.value->data()) || m.value->value()<low || m.value->value()>high)
            throw ConfigError("malformed_binding","/modbus/value","Register value out of range");
    } else {
        const auto p=std::find_if(action.parameters.begin(),action.parameters.end(),[&](const Parameter& p){return p.id==*m.argument;});
        if(action.parameters.size()!=1 || p==action.parameters.end() || !p->required ||
            p->type!=(m.function==5?"boolean":"integer") || (m.function==6 && (p->minimum<low || p->maximum>high)))
            throw ConfigError("malformed_binding","/modbus/argument","One required boolean/integer action parameter within the wire range is required");
    }
}
void validateModbusObservation(const ModbusObservation& m, const Connection* c, ExecutorKind kind, const Device& device) {
    modbusMapping(c,kind,m.address);
    if(m.function<1 || m.function>4)throw ConfigError("unsupported_executor","/modbus/function","Read functions 1..4 are supported");
    modbusType(m.dataType,m.function<=2);
    const auto f=std::find_if(device.state.begin(),device.state.end(),[&](const StateField& f){return f.id==m.field;});
    if(device.kind!=DeviceKind::Generic || f==device.state.end() || f->type!=(m.function<=2?"boolean":"integer"))
        throw ConfigError("malformed_binding","/modbus/field","Observation requires a declared property of the matching type");
}
}
