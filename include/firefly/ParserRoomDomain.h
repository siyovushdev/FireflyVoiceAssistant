#pragma once
#include <string>
#include <vector>
namespace firefly {
struct ParserRoomLocation {
    std::string phrase;
    std::string actionId;
    std::string targetId;
    std::string argumentId;
};
class ParserRoomDomain {
public:
    virtual ~ParserRoomDomain() = default;
    virtual std::vector<ParserRoomLocation> parserLocations() const = 0;
};
}
