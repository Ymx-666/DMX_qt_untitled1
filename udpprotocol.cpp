#ifndef UDPPROTOCOL_H
#define UDPPROTOCOL_H

#include <cstdint>

// 严格按 1 字节对齐，防止 C++ 编译器自动填充导致解析错位
#pragma pack(push, 1)

// 对应协议中的 JPG 压缩数据包头 (20字节)
struct ImagePacketHeader {
    uint16_t headerCode;    // 0-1字节: 固定码, 0xFFFF [cite: 20]
    uint16_t imageType;     // 2-3字节: 图像类型，0=黑白，1=彩色 [cite: 20]
    uint32_t timestamp;     // 4-7字节: 图像时间, 1970年以来的秒数 [cite: 20]
    uint32_t imageIndex;    // 8-11字节: 全景图像序号, 计数 [cite: 20]
    uint32_t totalSize;     // 12-15字节: 图像大小, 每幅压缩图像的大小 [cite: 20]
    uint16_t blockIndex;    // 16-17字节: 图像块计数, 0-65536 (注: 协议中写的是图像块计数，实际组包时通常代表当前块的序号) [cite: 20]
    uint16_t blockSize;     // 18-19字节: 图像块大小, 0-255 (注: 协议中写0-255，但后面又说最大8000，这里可能文档笔误，我们代码里用实际收到的包长度减去20即可) [cite: 20]
};

#pragma pack(pop)

#endif // UDPPROTOCOL_H
