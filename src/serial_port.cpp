#include "serial_port.hpp"
#include "logger.hpp" // 日志模块
#include <iostream>
#include <fcntl.h>   // 文件控制定义
#include <termios.h> // POSIX 终端控制定义
#include <unistd.h>  // UNIX 标准函数定义

SerialPort::SerialPort(const std::string& port_name, int baud_rate)
    : fd_(-1), port_name_(port_name), baud_rate_(baud_rate), is_open_(false), prev_retry_time_(std::chrono::steady_clock::now())
{

}

SerialPort::~SerialPort()
{
    ClosePort();
}

bool SerialPort::OpenPort()
{
    // O_RDWR: 读写模式 | O_NOCTTY: 不作为控制终端 | O_NDELAY: 非阻塞
    fd_ = open(port_name_.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    
    if (fd_ == -1)
    {
        LOG_ERROR("[serial_port.cpp] 无法打开串口 {}", port_name_);
        return false;
    }

    // 恢复串口为阻塞状态
    fcntl(fd_, F_SETFL, 0);

    // 配置串口参数
    struct termios options;
    tcgetattr(fd_, &options);

    // 设置波特率
    speed_t speed;
    switch (baud_rate_)
    {
        case 115200: speed = B115200; break;
        case 921600: speed = B921600; break;
        default:     speed = B115200; break; // 默认 115200
    }
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);

    // 8N1 模式: 8个数据位, 无奇偶校验, 1个停止位
    options.c_cflag &= ~PARENB;   // 无校验
    options.c_cflag &= ~CSTOPB;   // 1个停止位
    options.c_cflag &= ~CSIZE;    // 屏蔽数据位
    options.c_cflag |= CS8;       // 8个数据位
    
    // 禁用硬件流控
    options.c_cflag &= ~CRTSCTS;
    // 启用接收器，忽略调制解调器控制线
    options.c_cflag |= (CREAD | CLOCAL);

    // 禁用特殊字符处理和回显 (设为纯净的 Raw 模式)
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_iflag &= ~(INLCR | ICRNL | IGNCR);
    options.c_oflag &= ~OPOST;

    // 清空缓冲区并使设置生效
    tcflush(fd_, TCIFLUSH);
    if (tcsetattr(fd_, TCSANOW, &options) != 0)
    {
        LOG_ERROR("[serial_port.cpp] 串口参数设置失败！");
        close(fd_);
        return false;
    }

    is_open_ = true;
    LOG_INFO("[serial_port.cpp] 串口 {} 打开成功，波特率: {}", port_name_, baud_rate_);
    return true;
}

void SerialPort::ClosePort()
{
    if (is_open_ && fd_ != -1)
    {
        close(fd_);
        is_open_ = false;
        fd_ = -1;
        LOG_WARN("[serial_port.cpp] 串口已关闭。");
    }
}

bool SerialPort::SendData(const SendPacket& packet)
{
    if (!is_open_) 
    {
        LOG_WARN("[serial_port.cpp] 发送时发现串口未打开！");
        return false;
    }

    // 直接将结构体内存写入文件描述符
    ssize_t bytes_written = write(fd_, &packet, sizeof(SendPacket));
    
    if (bytes_written == sizeof(SendPacket))
    {
        return true;
    }
    else
    {
        LOG_ERROR("[serial_port.cpp] 数据发送不完整或失败！");

        // 尝试重新打开串口 reopenPort() 
        return ReopenPort();
    }
}

bool SerialPort::IsOpened()
{
    if (is_open_)
    {
        return true;
    }
    else
    {
        // 如果串口未打开，尝试每隔 0.50 秒重新打开一次
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - prev_retry_time_).count();

        if (elapsed >= 0.50)
        {
            prev_retry_time_ = now;
            ReopenPort();
        }
        return false;
    }
}

bool SerialPort::ReopenPort()
{
    LOG_WARN("[serial_port.cpp] 尝试重新打开串口...");
    ClosePort();
    return OpenPort();
}