#ifndef COLOR_HPP
#define COLOR_HPP

#include <string>

// ================= 终端输出颜色配置 =================

const std::string NONE   = "\033[0m";   // 重置为终端默认颜色
const std::string RED    = "\033[31m";  // 红色：用于严重错误
const std::string GREEN  = "\033[32m";  // 绿色：用于成功、性能指标
const std::string YELLOW = "\033[33m";  // 黄色：用于警告、断线重连
const std::string BLUE   = "\033[34m";  // 蓝色：用于普通信息提示
const std::string CYAN   = "\033[36m";  // 青色：用于串口发送数据等

#endif // COLOR_HPP