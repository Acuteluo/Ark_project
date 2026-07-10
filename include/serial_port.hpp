#ifndef SERIAL_PORT_HPP
#define SERIAL_PORT_HPP

#include <string>
#include <chrono>
#include "packet.hpp"

class SerialPort
{
public:
    SerialPort(const std::string& port_name, int baud_rate);
    ~SerialPort();

    // 打开串口并配置波特率等参数
    bool OpenPort();
    
    // 关闭串口
    void ClosePort();
    
    // 发送数据包
    bool SendData(const SendPacket& packet);
 
    // 检查串口是否处于打开状态，并处理重试逻辑
    bool IsOpened();

private:

    // 尝试重新打开串口
    bool ReopenPort();

    int fd_;                  // Linux 设备文件描述符
    std::string port_name_;   // 串口名称，从 config 里读取
    int baud_rate_;           // 波特率，从 config 里读取

    bool is_open_;            // 目前串口是否处于打开状态
    std::chrono::steady_clock::time_point prev_retry_time_;  // 上次尝试打开失败的时间
};

#endif // SERIAL_PORT_HPP