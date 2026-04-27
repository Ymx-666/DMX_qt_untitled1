#ifndef UDPPROTOCOL_H
#define UDPPROTOCOL_H

#include <cstdint>

#pragma pack(push, 1)
struct ImagePacketHeader {
    uint16_t headerCode;
    uint16_t imageType;
    uint32_t timestamp;
    uint32_t imageIndex;
    uint32_t totalSize;
    uint16_t blockIndex;
    uint16_t blockSize;
};
#pragma pack(pop)

#endif // UDPPROTOCOL_H
