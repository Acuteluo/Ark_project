#include "http_stream_thread.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

// ---------- 构造与析构 ----------
HttpStreamThread::HttpStreamThread(int port, int width, int height, int quality)
    : port_(port), width_(width), height_(height), quality_(quality),
      is_running_(false), has_new_frame_(false), server_sock_(-1),
      fps_frame_count_(0), fps_(0.0), fps_last_time_(std::chrono::steady_clock::now()) 
{

}

HttpStreamThread::~HttpStreamThread() 
{
    Stop();
}

// ---------- 启动与停止 ----------
void HttpStreamThread::Start() 
{
    if (is_running_) return;
    is_running_ = true;
    server_thread_ = std::thread(&HttpStreamThread::Run, this);
    LOG_INFO("[HttpStream] HTTP 服务器已启动，端口: {}", port_);
}

void HttpStreamThread::UpdateOverlayText(const std::string& text1, const std::string& text2) 
{
    std::lock_guard<std::mutex> lock(text_mutex_);
    overlay_text1_ = text1;
    overlay_text2_ = text2;
}

void HttpStreamThread::Stop() 
{
    if (!is_running_) return;
    is_running_ = false;

    // 关闭监听 socket，打破 accept 阻塞
    if (server_sock_ >= 0) 
    {
        shutdown(server_sock_, SHUT_RDWR);
        close(server_sock_);
        server_sock_ = -1;
    }

    // 唤醒所有等待的线程（通知条件变量）
    cv_.notify_all();

    if (server_thread_.joinable()) 
    {
        server_thread_.join();
    }
    LOG_INFO("[HttpStream] HTTP 服务器已停止。");
}

// ---------- 主线程更新帧 ----------
void HttpStreamThread::UpdateFrame(const cv::Mat& frame) {
    if (!is_running_ || frame.empty()) return;

    // try_lock 避免阻塞主线程（如果上一帧正在处理则跳过）
    if (mutex_.try_lock()) 
    {
        frame.copyTo(latest_frame_);
        has_new_frame_ = true;
        mutex_.unlock();
        cv_.notify_one();   // 唤醒发送线程
    }
}

// ---------- 服务器主循环 ----------
void HttpStreamThread::Run() 
{
    // 1. 创建 TCP socket
    server_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock_ < 0) 
    {
        LOG_ERROR("[HttpStream] 创建 socket 失败");
        is_running_ = false;
        return;
    }

    // 2. 允许端口复用（防止程序重启时被占用）
    int opt = 1;
    setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定地址和端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;   // 监听所有网卡
    addr.sin_port = htons(port_);

    if (bind(server_sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) 
    {
        LOG_ERROR("[HttpStream] 绑定端口 {} 失败", port_);
        close(server_sock_);
        server_sock_ = -1;
        is_running_ = false;
        return;
    }

    // 4. 开始监听
    if (listen(server_sock_, 5) < 0) 
    {
        LOG_ERROR("[HttpStream] 监听失败");
        close(server_sock_);
        server_sock_ = -1;
        is_running_ = false;
        return;
    }

    LOG_INFO("[HttpStream] 服务器正在 0.0.0.0:{} 监听", port_);

    // 5. 主循环：接受客户端连接
    while (is_running_) 
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock_, (struct sockaddr*)&client_addr, &client_len);

        if (client_sock < 0) 
        {
            if (is_running_) 
            {
                LOG_ERROR("[HttpStream] accept 失败");
            }
            continue;
        }

        // 每收到一个客户端，创建一个线程处理（可改为线程池，但简单够用）
        std::thread client_thread(&HttpStreamThread::HandleClient, this, client_sock);
        client_thread.detach();   // 分离，避免资源泄漏
    }
}

// ---------- 处理单个客户端 ----------
void HttpStreamThread::HandleClient(int client_sock) 
{
    // 1. 读取 HTTP 请求，只识别 GET /
    char buffer[1024];
    int n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) 
    {
        close(client_sock);
        return;
    }
    buffer[n] = '\0';

    // 如果不是 GET /，返回 404 并关闭
    if (strncmp(buffer, "GET /", 5) != 0) 
    {
        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client_sock, resp, strlen(resp), 0);
        close(client_sock);
        return;
    }

    // 2. 发送 HTTP 响应头（MJPEG 流）
    const char* header = "HTTP/1.1 200 OK\r\n"
                         "Connection: close\r\n"
                         "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n\r\n";
    if (send(client_sock, header, strlen(header), 0) < 0) 
    {
        close(client_sock);
        return;
    }

    // 3. 持续发送帧
    while (is_running_) 
    {
        cv::Mat frame;
        {
            // 等待新帧（超时 100ms，以便检查 is_running_）
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(100),
                              [this] { return has_new_frame_ || !is_running_; })) {
                continue;   // 超时继续循环
            }
            if (!is_running_) break;
            if (!has_new_frame_) continue;

            // 复制最新帧（为了不占用锁太久）
            frame = latest_frame_.clone();
            has_new_frame_ = false;
        }

        // 4. 缩放图像（按配置尺寸）
        cv::Mat resized;
        cv::resize(frame, resized, cv::Size(width_, height_));

        // ===== 新增：绘制 FPS =====
        // 更新帧计数
        fps_frame_count_++;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fps_last_time_).count();
        if (elapsed >= 0.50) 
        {
            fps_ = fps_frame_count_ / elapsed;
            fps_frame_count_ = 0;
            fps_last_time_ = now;
        }

        char buf[32];
        snprintf(buf, sizeof(buf), "FPS: %.1f", fps_);

        int fontFace = cv::FONT_HERSHEY_SIMPLEX;
        double fontScale = 1.0;
        int thickness = 2;
        int baseline;
        cv::Size textSize = cv::getTextSize(buf, fontFace, fontScale, thickness, &baseline);

        // 右上角：右边距 20 像素，上边距 20 像素
        int text_x = resized.cols - textSize.width - 20;
        int text_y = textSize.height + 20;   // 因为文字原点在基线，加上高度保证完全显示

        cv::putText(resized, buf, cv::Point(text_x, text_y), fontFace, fontScale, cv::Scalar(0, 165, 255), thickness);


        // 打印文本信息
        // ===== 绘制叠加文本（左下角）=====
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            cv::Scalar color(255, 255, 0);
            if (!overlay_text1_.empty()) 
            {
                cv::putText(resized, overlay_text1_, cv::Point(10, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, color, 2);
            }
            if (!overlay_text2_.empty()) 
            {
                cv::putText(resized, overlay_text2_, cv::Point(10, 60),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, color, 2);
            }
        }
        // ===== 结束叠加文本绘制 =====


        // 5. 编码为 JPEG
        std::vector<uchar> jpeg_buffer;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality_};
        if (!cv::imencode(".jpg", resized, jpeg_buffer, params)) 
        {
            LOG_ERROR("[HttpStream] JPEG 编码失败");
            continue;
        }

        // 6. 构造 MJPEG 片段（边界 + 头 + 数据 + 换行）
        std::string chunk_header = "--frame\r\n"
                                   "Content-Type: image/jpeg\r\n"
                                   "Content-Length: " + std::to_string(jpeg_buffer.size()) + "\r\n\r\n";

        // 发送片段头
        if (send(client_sock, chunk_header.c_str(), chunk_header.size(), 0) < 0) break;
        // 发送 JPEG 数据
        if (send(client_sock, reinterpret_cast<const char*>(jpeg_buffer.data()), jpeg_buffer.size(), 0) < 0) break;
        // 发送结束换行
        if (send(client_sock, "\r\n", 2, 0) < 0) break;

        // 7. 控制帧率（约 30fps，可根据需要调整）
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // 关闭连接
    close(client_sock);
}