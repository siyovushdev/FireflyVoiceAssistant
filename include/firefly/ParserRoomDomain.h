#pragma once
#include <string>
#include <vector>

namespace firefly {
// Only the existing parser's location slot and its legacy output code.
// No brightness, enablement, safety, persistence or request parsing API.
struct ParserRoomLocation {
    std::string phrase;
    char code;
};
class ParserRoomDomain {
public:
    virtual ~ParserRoomDomain() = default;
    virtual std::vector<ParserRoomLocation> parserLocations() const = 0;
};
}
