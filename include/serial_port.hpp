#ifndef SERIAL_PORT_HPP
#define SERIAL_PORT_HPP

#include <string>
#include "packet.hpp"

class SerialPort
{
public:
    SerialPort(const std::string& port_name, int baud_rate);
    ~SerialPort();

    // 打开串口并配置波特率等参数
    bool openPort();
    
    // 关闭串口
    void closePort();
    
    // 发送数据包
    bool sendData(const SendPacket& packet);
 
    // 检查串口是否处于打开状态
    bool isOpen() const;

private:
    int fd_;                  // Linux 设备文件描述符
    std::string port_name_;   // 串口名称，例如 "/dev/ttyUSB0"
    int baud_rate_;           // 波特率，例如 115200
    bool is_open_;
};

#endif // SERIAL_PORT_HPP