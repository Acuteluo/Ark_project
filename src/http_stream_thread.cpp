#include "http_stream_thread.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <sstream>   // 用于构建 HTML
#include <string>

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
    // 1. 读取 HTTP 请求行
    char buffer[1024];
    int n = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) 
    { 
        close(client_sock); 
        return; 
    }
    buffer[n] = '\0';

    // 2. 解析请求方法、路径
    char method[16], path[256];
    if (sscanf(buffer, "%s %s", method, path) != 2) 
    {
        close(client_sock);
        return;
    }
    // 只支持 GET
    if (strcmp(method, "GET") != 0) 
    {
        const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        send(client_sock, resp, strlen(resp), 0);
        close(client_sock);
        return;
    }

    // ========== 路径 /viewer ：返回带视频和文本的 HTML 页面 ==========
    if (strcmp(path, "/viewer") == 0) 
    {
        std::string html = R"raw(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Ark 图传</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        html, body {
            width: 100%;
            height: 100%;
            background: #1a1a1a;
            color: #fff;
            font-family: Arial, sans-serif;
            overflow: hidden;
            display: flex;
            flex-direction: column;
            padding: 6px;          /* 减小外边距，增加可用空间 */
        }
        #video-container {
            width: 100%;
            flex: 0 0 88%;          /* 从75%提升到88% */
            max-height: 88vh;
            background: #000;
            border-radius: 12px;
            overflow: hidden;
            display: flex;
            align-items: center;
            justify-content: center;
            box-shadow: 0 4px 20px rgba(0,0,0,0.8);
        }
        #video-container img {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: block;
        }
        #info-panel {
            width: 100%;
            flex: 0 0 auto;
            min-height: 28px;
            margin-top: 4px;        /* 减小上边距 */
            background: #2a2a2a;
            border-radius: 10px;
            border-left: 4px solid #ffaa00;
            padding: 3px 12px;
            text-align: left;
            font-size: 1rem;
            line-height: 1.4;
            box-shadow: 0 2px 10px rgba(0,0,0,0.5);
            overflow: hidden;
            display: flex;
            flex-direction: column;
            justify-content: center;
        }
        .info-line {
            margin: 1px 0;
            color: #ffcc66;
            font-weight: bold;
            word-break: break-all;
        }
        .info-error {
            color: #ff6666;
            font-weight: normal;
            font-size: 0.85rem;
        }
        @media (max-width: 480px) {
            body { padding: 4px; }
            #video-container { flex: 0 0 82%; }   /* 手机版也相应放大 */
            #info-panel { font-size: 0.85rem; padding: 2px 10px; min-height: 24px; margin-top: 3px; }
        }
    </style>
</head>
<body>
    <div id="video-container">
        <img src="/" alt="视频流" id="videoFeed" />
    </div>
    <div id="info-panel">
        <div class="info-line" id="info1">等待数据...</div>
        <div class="info-line" id="info2">等待数据...</div>
    </div>

    <script>
        (function() {
            const el1 = document.getElementById('info1');
            const el2 = document.getElementById('info2');

            function fetchInfo() {
                fetch('/info', { cache: 'no-store' })
                    .then(res => {
                        if (!res.ok) throw new Error('HTTP ' + res.status);
                        return res.json();
                    })
                    .then(data => {
                        el1.textContent = data.info1 || '—';
                        el2.textContent = data.info2 || '—';
                        el1.className = 'info-line';
                        el2.className = 'info-line';
                    })
                    .catch(err => {
                        el1.textContent = '⚠️ 错误: ' + err.message;
                        el2.textContent = '尝试刷新';
                        el1.className = 'info-line info-error';
                        el2.className = 'info-line info-error';
                        console.error('[Ark] fetch error:', err);
                    });
            }

            setInterval(fetchInfo, 150);
            fetchInfo();
            document.addEventListener('visibilitychange', function() {
                if (!document.hidden) fetchInfo();
            });
        })();
    </script>
</body>
</html>
)raw";

        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html\r\n"
                               "Content-Length: " + std::to_string(html.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + html;
        send(client_sock, response.c_str(), response.size(), 0);
        close(client_sock);
        return;
    }

    // ========== 路径 /info ：返回 JSON 字符串 ==========
    if (strcmp(path, "/info") == 0) 
    {
        std::string info1, info2;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            info1 = overlay_text1_;
            info2 = overlay_text2_;
        }
        // 转义 JSON 中的特殊字符（简单处理，若包含引号则需要转义）
        std::string json = "{\"info1\":\"" + info1 + "\",\"info2\":\"" + info2 + "\"}";
        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(json.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + json;
        send(client_sock, response.c_str(), response.size(), 0);
        close(client_sock);
        return;
    }

    // ========== 路径 / ：原有视频流 ==========
    if (strcmp(path, "/") == 0) 
    {
        // 原有视频流处理逻辑（完全保留你现在的代码，从发送响应头开始）
        const char* header = "HTTP/1.1 200 OK\r\n"
                             "Connection: close\r\n"
                             "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n\r\n";
        if (send(client_sock, header, strlen(header), 0) < 0) 
        {
            close(client_sock);
            return;
        }

        while (is_running_) 
        {
            cv::Mat frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!cv_.wait_for(lock, std::chrono::milliseconds(100),
                                  [this] { return has_new_frame_ || !is_running_; })) {
                    continue;
                }
                if (!is_running_) break;
                if (!has_new_frame_) continue;
                frame = latest_frame_.clone();
                has_new_frame_ = false;
            }

            cv::Mat rotated;
            cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);  // 逆时针旋转90度
            cv::Mat resized;
            cv::resize(rotated, resized, cv::Size(height_, width_));      // 宽高互换

            // ----- 绘制 FPS 和叠加文本（同你的现有代码）-----
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
            cv::putText(resized, buf, 
                cv::Point(resized.cols - 115, 35),  // 微调Y坐标
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 165, 255), 2);

            {
                std::lock_guard<std::mutex> lock(text_mutex_);
                if (!overlay_text1_.empty()) 
                {
                    cv::putText(resized, overlay_text1_, cv::Point(10, 35),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 0), 2);
                }
                if (!overlay_text2_.empty()) 
                {
                    cv::putText(resized, overlay_text2_, cv::Point(10, 70),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 0), 2);
                }
            }
            // ------------------------------------------------

            std::vector<uchar> jpeg_buffer;
            std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality_};
            if (!cv::imencode(".jpg", resized, jpeg_buffer, params)) 
            {
                LOG_ERROR("[HttpStream] JPEG 编码失败");
                continue;
            }

            std::string chunk_header = "--frame\r\n"
                                       "Content-Type: image/jpeg\r\n"
                                       "Content-Length: " + std::to_string(jpeg_buffer.size()) + "\r\n\r\n";
            if (send(client_sock, chunk_header.c_str(), chunk_header.size(), 0) < 0) break;
            if (send(client_sock, reinterpret_cast<const char*>(jpeg_buffer.data()), jpeg_buffer.size(), 0) < 0) break;
            if (send(client_sock, "\r\n", 2, 0) < 0) break;

            // 控制帧率（可调）
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        close(client_sock);
        return;
    }

    // ========== 其他路径 404 ==========
    const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send(client_sock, resp, strlen(resp), 0);
    close(client_sock);
}