#include <opencv2/opencv.hpp>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#include "qrcode_scanner.hpp"
#include "serial_port.hpp"


// ================= 全局共享数据与锁 =================
struct SharedData
{
    cv::Mat current_frame;         // 主线程写入：当前刚捕获的相机原图
    cv::Mat display_frame;         // 视觉线程写入：画好橙色边框的最终渲染图

    bool frame_updated = false;    // 标志位：是否有新图
    bool is_4_qr_ready = false;    // 标志位：是否集齐了 4 个二维码

    // 将解码出的信息存在这里供串口线程计算使用
    std::vector<std::string> qr_infos; 

    // 记录系统的启动时刻与集齐 4 个二维码的结束时刻
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
};

SharedData g_data;
std::mutex g_mtx;
std::condition_variable g_cv;
std::atomic<bool> g_running{true}; // 控制所有线程退出的原子变量

// ================= 子线程 A: 视觉识别线程 =================
void VisionThread(QRCodeScanner* scanner)
{
    cv::Mat process_frame;

    while (g_running)
    {
        {
            // 等待主线程塞入新图片
            std::unique_lock<std::mutex> lock(g_mtx);
            g_cv.wait(lock, [] { return g_data.frame_updated || !g_running; });

            if (!g_running) break;

            // 拷贝图片到本线程，然后立即释放锁，不阻塞主线程读相机
            process_frame = g_data.current_frame.clone();
            g_data.frame_updated = false;
        }

        // 进行二维码识别
        if (!process_frame.empty())
        {
            // 处理图像（画框、存字符串操作均在内部完成，直接修改了 process_frame）
            scanner->processFrame(process_frame);

            // 再次加锁，将画好框的图传回给主线程用于显示，同时更新状态
            std::lock_guard<std::mutex> lock(g_mtx);
            g_data.display_frame = process_frame.clone(); // 传回 UI 渲染图

            // 如果刚好集齐 4 个，且尚未触发过 4 个二维码的 ready
            if (scanner->getInfos().size() == 4 && !g_data.is_4_qr_ready) 
            {
                g_data.is_4_qr_ready = true;
                g_data.qr_infos = scanner->getInfos(); // 把字符串数组拷贝进共享内存

                // 获取当前时间，并计算与 start_time 的差值
                g_data.end_time = std::chrono::steady_clock::now();
                double elapsed_seconds = std::chrono::duration<double>(g_data.end_time - g_data.start_time).count();

                std::cout << "[VISION THREAD] 成功集齐 4 个二维码，准备让串口线程发送" << std::endl;
                std::cout << "[PERFORMANCE] 从系统启动到集齐 4 个二维码耗时: " << elapsed_seconds << " s" << std::endl;
            }
        }
    }
}



// ================= 子线程 B: 串口发送线程 =================
void SerialThread(SerialPort* serial)
{
    SendPacket packet;

    // 初始化数据包，默认状态 0x00
    packet.data = 0x00; 

    std::vector<std::string> local_infos; // 暂存从共享内存拿出的字符串

    while (g_running)
    {
        bool ready_to_send = false;

        {
            // 加锁快速读取标志位
            std::lock_guard<std::mutex> lock(g_mtx);
            ready_to_send = g_data.is_4_qr_ready;

            if (ready_to_send)
            {
                std::cout << "[VISION THREAD] 集齐了 4 个二维码，准备发送！" << std::endl;
                local_infos = g_data.qr_infos; // 安全地将字符串数组拷贝出来
            }
        }

        // 如果集齐了 4 个二维码，且串口已打开
        if (serial->isOpen())
        {
            if (ready_to_send)
            {
                packet.data = 0x01; // 代表状态 OK
            }
            else
            {
                packet.data = 0x09; // 不 ok    
            }

            serial->sendData(packet);
            std::cout << "[SERIAL] 发送了: " << std::hex << static_cast<int>(packet.data) << std::dec << std::endl;
        }
        else
        {
            std::cout << "[SERIAL] 串口线程发现串口未打开" << std::endl;
        }
        

        // 控制发送频率为 100Hz (休眠 10ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}   



int main()
{
    // 1. 读取配置文件
    std::string config_path = "../config.yaml";
    cv::FileStorage fs(config_path, cv::FileStorage::READ);
    
    if (!fs.isOpened())
    {
        std::cerr << "[ERROR] 无法打开配置文件: " << config_path << std::endl;
        return -1;
    }

    // 解析相机参数
    int camera_id = (int)fs["Camera"]["DeviceID"];
    int cam_width = (int)fs["Camera"]["Width"];
    int cam_height = (int)fs["Camera"]["Height"];
    int cam_fps = (int)fs["Camera"]["FPS"];
    std::string cam_format = (std::string)fs["Camera"]["Format"];

    // 解析模型路径
    std::string detect_prototxt = (std::string)fs["Model"]["DetectPrototxt"];
    std::string detect_caffe = (std::string)fs["Model"]["DetectCaffe"];
    std::string sr_prototxt = (std::string)fs["Model"]["SrPrototxt"];
    std::string sr_caffe = (std::string)fs["Model"]["SrCaffe"];
    fs.release();

    std::cout << "================= [CONFIG LOADED] =================" << std::endl;
    std::cout << "  Camera ID : " << camera_id << std::endl;
    std::cout << "  Target Res: " << cam_width << "x" << cam_height << " @ " << cam_fps << " FPS" << std::endl;
    std::cout << "  Target Fmt: " << cam_format << std::endl;
    std::cout << "  Model Path: " << detect_prototxt << " (等4个文件)" << std::endl;
    std::cout << "===================================================" << std::endl;

    // 2. 初始化二维码扫描器
    QRCodeScanner scanner;
    if (!scanner.initModel(detect_prototxt, detect_caffe, sr_prototxt, sr_caffe))
    {
        std::cerr << "[ERROR] 扫描器初始化失败，程序退出。" << std::endl;
        return -1;
    }

    // 3. 初始化摄像头
    cv::VideoCapture cap(camera_id);
    if (!cap.isOpened())
    {
        std::cerr << "[ERROR] 无法打开摄像头, ID: " << camera_id << std::endl;
        return -1;
    }

    if (cam_format == "MJPG") 
    {
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, cam_width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, cam_height);
    cap.set(cv::CAP_PROP_FPS, cam_fps);

    // ==========================================================
    // 全面获取并打印相机实际生效的参数
    // ==========================================================
    int actual_width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int actual_height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
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
    double auto_focus = cap.get(cv::CAP_PROP_AUTOFOCUS);
    double exposure = cap.get(cv::CAP_PROP_EXPOSURE);
    double brightness = cap.get(cv::CAP_PROP_BRIGHTNESS);
    double contrast = cap.get(cv::CAP_PROP_CONTRAST);
    std::string backend_name = cap.getBackendName(); // 例如 V4L2

    std::cout << std::endl;
    std::cout << "================= [CAMERA STATUS] =================" << std::endl;
    std::cout << "  Backend API   : " << backend_name << std::endl;
    std::cout << "  Resolution    : " << actual_width << "x" << actual_height << std::endl;
    std::cout << "  FPS           : " << actual_fps << std::endl;
    std::cout << "  Video Format  : " << (strlen(actual_fourcc) > 0 ? actual_fourcc : "Unknown") << std::endl;
    std::cout << "  Auto Focus    : " << (auto_focus == 1 ? "ON" : (auto_focus == 0 ? "OFF" : "Not Supported/Unknown")) << std::endl;
    std::cout << "  Exposure      : " << exposure << std::endl;
    std::cout << "  Brightness    : " << brightness << std::endl;
    std::cout << "  Contrast      : " << contrast << std::endl;
    std::cout << "===================================================" << std::endl;

    std::cout << "[INFO] 相机初始化完毕。按 'ESC' 或 'q' 退出" << std::endl;

    // 4. 初始化串口
    std::string port_name = "/dev/ttyACM0";
    int baud_rate = 115200;
    SerialPort serial(port_name, baud_rate);
    
    std::cout << "\n================= [SERIAL STATUS] =================" << std::endl;
    bool is_serial_open = serial.openPort();
    if (is_serial_open)
    {
        std::cout << "  Port Name     : " << port_name << std::endl;
        std::cout << "  Baud Rate     : " << baud_rate << std::endl;
        std::cout << "  Status        : SUCCESS (Ready to TX/RX)" << std::endl;
    }
    else
    {
        std::cerr << "  Status        : FAILED! Please check USB connection or dialout permissions." << std::endl;
        std::cerr << "  Notice        : The system will run in Vision-Only mode." << std::endl;
    }
    std::cout << "===================================================" << std::endl << std::endl;

     
    
    // 5. 启动子线程  
    std::thread vision_thread(VisionThread, &scanner); // 启动视觉识别线程
    std::thread serial_thread(SerialThread, &serial);  // 启动串口发送线程 

    std::cout << "[INFO] 系统启动！相机、视觉、串口多线程运行中..." << std::endl;

    // 启动计时
    g_data.start_time = std::chrono::steady_clock::now();

    // 6. 主循环
    cv::Mat frame;
    cv::Mat show_frame; // 显示用图
    while (g_running)
    {
        cap >> frame;

        // 丢进共享内存，通知视觉线程
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_data.current_frame = frame;
            g_data.frame_updated = true;

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
        g_cv.notify_one();


        // 缩放画面以便在普通笔记本屏幕上完整显示 1080p 图像
        // cv::Mat display_frame;
        // cv::resize(show_frame, display_frame, cv::Size(960, 540));

        // 显示结果
        cv::imshow("Ark System", show_frame);

        // 处理键盘交互
        char key = (char)cv::waitKey(1);
        if (key == 27 || key == 'q' || key == 'Q')
        {
            g_running = false;
        }
    }

    // 7. 通知所有线程起床并结束
    g_running = false; // 【双保险】：跳出循环后立刻全剧终
    g_cv.notify_all();
    if (vision_thread.joinable()) vision_thread.join();
    if (serial_thread.joinable()) serial_thread.join();

    // 8. 释放资源
    cap.release();
    cv::destroyAllWindows();

    return 0;
}