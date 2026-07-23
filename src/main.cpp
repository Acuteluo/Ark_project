#include <opencv2/opencv.hpp>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <csignal> // 引入信号处理，用于树莓派无头模式下的 Ctrl+C 退出

#include "qrcode_scanner.hpp" // 二维码识别
#include "serial_port.hpp"    // 串口通信
#include "logger.hpp"         // 日志模块
#include "stream_thread.hpp"  // 图传模块

// ================= 运行模式配置 =================
// true : 模式① 电脑端联调模式（显示UI画面，带有深拷贝与画框）
// false: 模式② 树莓派部署模式（无UI，极致零拷贝，不进行画面渲染）
bool g_show_gui;

// ================= 全局共享数据与锁 =================
struct SharedData
{
    cv::Mat current_frame;         // 主线程写入：当前刚捕获的相机原图
    cv::Mat display_frame;         // 视觉线程写入：画好橙色边框的最终渲染图

    bool frame_updated = false;    // 标志位：是否有新图
    bool is_camera_connected = false; // 标志位：相机是否连接

    // 将解码出的信息存在这里供串口线程计算使用
    std::pair<uint8_t, uint8_t> qr_infos = std::make_pair(0xFF, 0xFF); 
};

SharedData g_data;
std::mutex g_mtx;
std::condition_variable g_cv;
std::atomic<bool> g_running{true}; // 控制所有线程退出的原子变量

// 捕获 Ctrl+C 信号，确保无UI模式下能优雅退出
void signalHandler(int signum)
{
    LOG_DEBUG("\n");
    LOG_WARN("[main.cpp] 捕捉到退出信号(SIGINT)，准备退出程序...\n");
    g_running = false;
}

// ================= 子线程 A: 视觉识别线程 =================
// 把 streamer 作为参数传进来，用于图像流
void VisionThread(QRCodeScanner* scanner, std::shared_ptr<StreamThread> streamer)
{
    cv::Mat process_frame;

    // --- 【新增】帧率统计相关变量 ---
    int frame_count = 0;
    auto fps_start_time = std::chrono::steady_clock::now();

    while (g_running)
    {
        {
            // 等待主线程塞入新图片
            std::unique_lock<std::mutex> lock(g_mtx);
            g_cv.wait(lock, [] { return g_data.frame_updated || !g_running; });

            if (!g_running) break;

            if (g_show_gui) 
            {
                // 电脑模式：深拷贝图片到本线程，不阻塞主线程读相机，保留用于显示的帧
                process_frame = g_data.current_frame.clone();
            } 
            else 
            {
                // 树莓派模式：【零拷贝】直接交换指针，极大节省时间和CPU
                cv::swap(process_frame, g_data.current_frame);
            }

            g_data.frame_updated = false;
        }

        // 进行二维码识别
        if (!process_frame.empty())
        {
            // 处理图像，如果 g_show_gui 那就直接修改
            // 修改：无论如何都给图片进行渲染，否则回传原图看不见信息
            scanner->ProcessFrame(process_frame, true);

            // 把处理完（带框）的图像丢给图传线程！
            // 因为 UpdateFrame 内部用的是 try_lock，哪怕图传那边因为网络波动卡住了，
            // 这里也会瞬间放弃并往下走，绝不拖累视觉处理的帧率！
            if (streamer) 
            {
                streamer->UpdateFrame(process_frame);
            }

            // 如果在电脑模式下，就再次加锁，将画好框的图传回给主线程用于显示，同时更新状态
            if (g_show_gui) 
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_data.display_frame = process_frame.clone(); // 传回 UI 渲染图
            }

            // 获取当前时间，并计算与 start_time 的差值
            ++frame_count;
            auto fps_current_time = std::chrono::steady_clock::now();
            double elapsed_seconds = std::chrono::duration<double>(fps_current_time - fps_start_time).count();
            if (elapsed_seconds >= 1.00) 
            {
                double fps = frame_count / elapsed_seconds;
                // LOG_DEBUG("\n");
                // LOG_INFO("[main.cpp] 当前计算帧率: {:.2f} FPS", fps);
                frame_count = 0;
                fps_start_time = fps_current_time;
            }

            // 把有效信息拷贝进共享内存，必须加锁！
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_data.qr_infos = scanner->GetResult(); 
            }
        }
    }
}



// ================= 子线程 B: 串口发送线程 =================
void SerialThread(SerialPort* serial)
{
    SendPacket packet;

    std::string prev_message = "", temp_message = ""; // 上一次打印的提示信息，这一次打印的提示信息
    std::pair<uint8_t, uint8_t> local_infos = {0x00, 0x00}; // 暂存从共享内存拿出的 uint8_t 

    while (g_running)
    {
        bool have_valid_infos = false; // 是否检测到有效信息
        bool is_camera_ok = false;     // 相机是否正常连接
        bool send_feedback = false;    // 是否把要发送的信息写入底层

        // 1. 拿取数据
        {
            // 加锁快速读取标志位
            std::lock_guard<std::mutex> lock(g_mtx);
            
            local_infos = g_data.qr_infos; // 安全地将字符串数组拷贝出来
            have_valid_infos = (local_infos.first != 0xFF) || (local_infos.second != 0xFF);
            is_camera_ok = g_data.is_camera_connected;
        }

        // 2. 先根据 is_camera_ok 再根据 have_valid_infos 的状态来设置数据包内容
        if (!is_camera_ok)
        {
            packet.data1 = 0xEE; // 相机未连接
            packet.data2 = 0xEE;
        }
        else if (have_valid_infos)
        {
            packet.data1 = local_infos.first; // 代表状态 OK
            packet.data2 = local_infos.second;
        }
        else
        {
            packet.data1 = 0xFF; // 不 ok
            packet.data2 = 0xFF;
        }


        // 3.发送数据, 如果串口已打开就发送！
        if (serial->IsOpened())
        {
            send_feedback = serial->SendData(packet);
        }

        // 4. 组合提示信息
        std::string camera_status = std::string(is_camera_ok ? "OK" : "ERROR");
        std::string serial_status = std::string(serial->IsOpened() ? "OK" : "ERROR");
        std::string valid_infos_status = std::string(have_valid_infos ? "YES" : "NO");
        std::string send_status = std::string(send_feedback ? "SUCCESSED" : "FAILED");

        temp_message = "[SERIAL THREAD] 相机 " 
            + camera_status
            + ", 串口 " + serial_status
            + ", 有效信息 " + valid_infos_status
            + ", 发送 " + std::to_string(static_cast<int>(packet.data1)) + " and " + std::to_string(static_cast<int>(packet.data2))
            + " " + send_status;
        
        // 假如这一次和上一次打印的提示信息不一样，就打印出来，一样就不打印，避免重复刷屏
        if (temp_message != prev_message)
        {
            LOG_INFO("{}", temp_message.c_str());
            prev_message = temp_message;
        }

        // 控制发送频率为 100Hz (休眠 10ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}   



int main()
{
    // 配置全局 logger 输出等级
    tools::logger()->set_level(spdlog::level::debug);

    // 注册系统信号，方便在树莓派后台运行时 Ctrl+C 正常关闭硬件
    signal(SIGINT, signalHandler);

    // 1. 读取配置文件
    std::string config_path = "../config.yaml";
    cv::FileStorage fs(config_path, cv::FileStorage::READ);
    
    if (!fs.isOpened())
    {
        LOG_ERROR("[main.cpp] 无法打开配置文件: {}", config_path);
        return -1;
    }

    // 解析 config 模式参数
    g_show_gui = (int)fs["Mode"]["PCMode"];

    // 解析 config 相机参数
    int camera_id = (int)fs["Camera"]["DeviceID"];
    int cam_width = (int)fs["Camera"]["Width"];
    int cam_height = (int)fs["Camera"]["Height"];
    int cam_brightness = (int)fs["Camera"]["Brightness"];
    int cam_contrast = (int)fs["Camera"]["Contrast"];
    std::string cam_format = (std::string)fs["Camera"]["Format"];

    // 解析模型路径
    std::string detect_prototxt = (std::string)fs["Model"]["DetectPrototxt"];
    std::string detect_caffe = (std::string)fs["Model"]["DetectCaffe"];
    std::string sr_prototxt = (std::string)fs["Model"]["SrPrototxt"];
    std::string sr_caffe = (std::string)fs["Model"]["SrCaffe"];

    // 解析串口参数
    std::string port_name = (std::string)fs["Port"]["PortName"];
    int baud_rate = (int)fs["Port"]["BaudRate"];
    SerialPort serial(port_name, baud_rate);

    // 解析图传参数
    int stream_port = (int)fs["Stream"]["Port"];
    int stream_width = (int)fs["Stream"]["Width"];
    int stream_height = (int)fs["Stream"]["Height"];
    int stream_quality = (int)fs["Stream"]["Quality"];

    fs.release();

    LOG_DEBUG("================= [CONFIG LOADED] =================");
    LOG_DEBUG("  Mode: {}", g_show_gui ? "PC" : "Raspberry Pi");
    LOG_DEBUG("  Camera ID : {}", camera_id);
    LOG_DEBUG("  Target Res: {}x{}", cam_width, cam_height);
    LOG_DEBUG("  Target Fmt: {}", cam_format);
    LOG_DEBUG("  Target Brightness: {}", cam_brightness);
    LOG_DEBUG("  Target Contrast: {}", cam_contrast);
    LOG_DEBUG("  Model Path: {} | {} | {} | {}\n", detect_prototxt, detect_caffe, sr_prototxt, sr_caffe);
    LOG_DEBUG("  Stream        : Port={}, Res={}x{}, Quality={}", stream_port, stream_width, stream_height, stream_quality);
    LOG_DEBUG("===================================================");


    // 2. 初始化二维码扫描器
    QRCodeScanner scanner;
    if (!scanner.InitModel(detect_prototxt, detect_caffe, sr_prototxt, sr_caffe))
    {
        LOG_ERROR("[main.cpp] QRCodeScanner scanner initialization failed, exiting program.");
        return -1;
    }

    // 3. 初始化摄像头
    cv::VideoCapture cap;

    auto openCamera = [&]() -> bool {
        if (cap.isOpened())
        {
            cap.release(); // 先释放，避免之前的实例占用
        }
        cap.open(camera_id);

        if (!cap.isOpened())
        {
            return false;
        }

        if (cam_format == "MJPG") 
        {
            cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        }
        cap.set(cv::CAP_PROP_FRAME_WIDTH, cam_width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, cam_height);
        cap.set(cv::CAP_PROP_BRIGHTNESS, cam_brightness); // [-64, 64]
        cap.set(cv::CAP_PROP_CONTRAST, cam_contrast);   // [0, 95]

        // ==========================================================
        // 全面获取并打印相机实际生效的参数
        // ==========================================================
        int actual_width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
        int actual_height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        int actual_brightness = cap.get(cv::CAP_PROP_BRIGHTNESS);
        int actual_contrast = cap.get(cv::CAP_PROP_CONTRAST);
        double actual_fps = cap.get(cv::CAP_PROP_FPS);
        
        // 获取实际的 FourCC 格式并转换为字符串
        int ex = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
        char actual_fourcc[] = {
            (char)(ex & 0XFF),
            (char)((ex & 0XFF00) >> 8),
            (char)((ex & 0XFF0000) >> 16),
            (char)((ex & 0XFF000000) >> 24),
            '\0'
        }; 

        // 获取其他可能有用的图像控制参数 (不同相机支持度不同，不支持通常返回 -1 或 0)
        double exposure = cap.get(cv::CAP_PROP_EXPOSURE);
        std::string backend_name = cap.getBackendName(); // 例如 V4L2

        LOG_DEBUG("");
        LOG_DEBUG("================= [ACTUAL CAMERA STATUS] =================");
        LOG_DEBUG("  Backend API   : {}", backend_name);
        LOG_DEBUG("  Resolution    : {}x{}", actual_width, actual_height);
        LOG_DEBUG("  FPS           : {}", actual_fps);
        LOG_DEBUG("  Video Format  : {}", (strlen(actual_fourcc) > 0 ? actual_fourcc : "Unknown"));
        LOG_DEBUG("  Exposure      : {}", exposure);
        LOG_DEBUG("  Brightness    : {}", actual_brightness);
        LOG_DEBUG("  Contrast      : {}", actual_contrast);
        LOG_DEBUG("==========================================================");
        LOG_DEBUG("[main.cpp] 相机初始化完毕, {}\n", (g_show_gui ? "按 'ESC' 或 'q' 退出" : "按 'Ctrl+C' 退出"));

        return true;
    };

    if (!openCamera())
    {
        LOG_ERROR("[main.cpp] 初始打开相机失败, ID: {}", camera_id);
        g_data.is_camera_connected = false;
        // 这里不再 return -1 直接退出程序，而是让它进入下方的主循环，自动开始 0.5s 间隔的重连
    }
    else
    {
        g_data.is_camera_connected = true; // 摄像头成功打开，设置标志位为 true
    }

    // 3.new 实例化图传线程
    // 使用 10.42.0.255 作为广播地址。只要你的笔记本连上了 Ark_Vision 热点，
    // 监听 8888 端口就能收到图传，无论笔记本被分配了什么 IP。
    std::shared_ptr<StreamThread> streamer = 
        std::make_shared<StreamThread>("10.42.0.255", stream_port, stream_width, stream_height, stream_quality);

    // 4. 读取串口配置，并初始化串口
    
    LOG_DEBUG("================= [SERIAL STATUS] =================");
    bool is_serial_open = serial.OpenPort();
    if (is_serial_open)
    {
        LOG_INFO("  is_serial_open: SUCCESS");
        LOG_DEBUG("  Port Name     : {}", port_name);
        LOG_DEBUG("  Baud Rate     : {}", baud_rate);
    }
    else
    {
        LOG_ERROR("  is_serial_open: FAILED");
    }
    LOG_DEBUG("===================================================\n");

     
    // 5. 启动子线程  
    std::thread vision_thread(VisionThread, &scanner, streamer); // 启动视觉识别线程，把 streamer 传进 vision_thread
    std::thread serial_thread(SerialThread, &serial);  // 启动串口发送线程 
    streamer->Start(); // 启动图传线程

    LOG_DEBUG("[main.cpp] starting 3 threads...\n");


    // 6. 主循环
    cv::Mat frame;
    cv::Mat show_frame; // 显示用图
    int empty_frame_count = 0; // 连续空帧计数器，用于判断相机是否被物理拔出

    while (g_running)
    {
        if (!g_data.is_camera_connected)
        {
            LOG_WARN("[main.cpp] 相机已断开, 0.5s 后尝试重新连接...");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            if (openCamera()) 
            {
                LOG_INFO("[main.cpp] 相机重连成功，恢复视觉流！");
                std::lock_guard<std::mutex> lock(g_mtx);
                g_data.is_camera_connected = true;
                empty_frame_count = 0; // 重置防抖计数
            }
            continue; // 重连阶段，直接跳过本轮后续的图像捕获与共享内存写入
        }

        cap >> frame;

        if (frame.empty()) 
        {
            empty_frame_count++;

            // 连续 5 帧拉取不到图像，才判定为相机被物理拔出（机器人行进中由于震动导致的偶发掉帧）
            if (empty_frame_count >= 5) 
            {
                LOG_ERROR("[main.cpp] 连续读取空帧，相机物理掉线！");
                std::lock_guard<std::mutex> lock(g_mtx);
                g_data.is_camera_connected = false; // 触发掉线
            }
            continue; 
        }
        else 
        {
            empty_frame_count = 0; // 只要读到一帧有效图，立刻清零防抖计数器
        }

        // 丢进共享内存，通知视觉线程
        {
            std::lock_guard<std::mutex> lock(g_mtx);

            if (g_show_gui) 
            {
                // UI模式：必须拷贝，保证识别线程画图时不污染原始流
                g_data.current_frame = frame.clone();
                
                // 如果视觉线程已经处理好了一帧带框的图，就显示带框的，否则显示原图
                if (!g_data.display_frame.empty()) 
                {
                    show_frame = g_data.display_frame.clone();
                } 
                else 
                {
                    show_frame = frame.clone();
                }
            } 
            else 
            {
                // 树莓派模式：0拷贝，底层直接接管指针。速度最快。
                cv::swap(g_data.current_frame, frame);
            }
            
            g_data.frame_updated = true;
        }
        g_cv.notify_one();


        // 根据模式判断是否显示画面
        if (g_show_gui) 
        {
            // 显示结果
            if (!show_frame.empty()) 
            {
                cv::imshow("Ark System", show_frame);
            }

            // 处理键盘交互
            char key = (char)cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q')
            {
                g_running = false;
            }
        }
    }

    // 7. 通知所有线程起床并结束
    g_running = false; // 【双保险】：跳出循环后立刻全剧终
    g_cv.notify_all();
    if (vision_thread.joinable()) vision_thread.join();
    if (serial_thread.joinable()) serial_thread.join();

    // 8. 释放资源
    cap.release();
    if (g_show_gui)
    {
        cv::destroyAllWindows();
    }

    // 【新增】安全停掉图传
    streamer->Stop();
    
    return 0;
}