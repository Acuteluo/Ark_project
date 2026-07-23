cmake_minimum_required(VERSION 3.10)
project(Ark_project)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 寻找 OpenCV 库 (必须包含 wechat_qrcode 模块)
find_package(OpenCV REQUIRED)

# 寻找线程库 (pthread)
set(THREADS_PREFER_PTHREAD_FLAG ON) 
find_package(Threads REQUIRED)

# 寻找 spdlog
find_package(spdlog REQUIRED)

# 包含目录
include_directories(
    include
    ${OpenCV_INCLUDE_DIRS}
)

# 生成可执行文件
add_executable(Ark_Scanner 
    src/logger.cpp
    src/main.cpp 
    src/qrcode_scanner.cpp
    src/serial_port.cpp 
    src/stream_thread.cpp
)

# 链接库
target_link_libraries(Ark_Scanner 
    ${OpenCV_LIBS}
    Threads::Threads
    spdlog::spdlog
)

install(DIRECTORY models DESTINATION share/${PROJECT_NAME})