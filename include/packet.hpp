#ifndef PACKET_HPP
#define PACKET_HPP

#include <cstdint>

// 强制字节对齐，防止 C++ 编译器自动填充内存导致电控解析错位
#pragma pack(push, 1)

struct SendPacket
{
    uint8_t header = 0xEF; // 帧头 
    
    // --- 有效数据起 ---

    uint8_t data1;         
    uint8_t data2;
    
    // --- 有效数据止 ---

    uint8_t tail = 0xFE; // 帧尾
};

#pragma pack(pop)

#endif // PACKET_HPP