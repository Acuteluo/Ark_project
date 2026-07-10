#ifndef PACKET_HPP
#define PACKET_HPP

#include <cstdint>

// 强制字节对齐，防止 C++ 编译器自动填充内存导致电控解析错位
#pragma pack(push, 1)

struct SendPacket
{
    uint8_t header = 0xEF; // 帧头 
    
    // --- 有效数据起 ---
    // 暂定发送 1 个 uint8_t 数据，后期可根据具体需求修改为其他定长数组
    uint8_t data;         
    // --- 有效数据止 ---

    uint8_t tail = 0xFE; // 帧尾
};

#pragma pack(pop)

#endif // PACKET_HPP