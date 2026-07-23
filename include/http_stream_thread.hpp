#ifndef HTTP_STREAM_THREAD_HPP
#define HTTP_STREAM_THREAD_HPP

#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>
#include <condition_variable>
#include "logger.hpp"

/**
 * @brief HTTP MJPEG 流推送线程
 * 
 * 监听指定端口，收到 HTTP GET 请求后，持续发送最新帧的 JPEG 数据。
 * 完全自包含，不依赖第三方库。
 */
class HttpStreamThread {
public:
    /**
     * @param port     监听端口
     * @param width    发送图像的宽度（缩放至该尺寸）
     * @param height   发送图像的高度
     * @param quality  JPEG 压缩质量 (0-100)
     */
    HttpStreamThread(int port, int width, int height, int quality);
    ~HttpStreamThread();

    /** 启动 HTTP 服务器（非阻塞，另起线程） */
    void Start();

    /** 停止服务器并回收资源 */
    void Stop();

    /** 
     * @brief 主线程调用，更新最新一帧（非阻塞）
     * @param frame 最新图像
     */
    void UpdateFrame(const cv::Mat& frame);

    /**
     * @brief 更新画面上的叠加文本（线程安全）
     * @param text1 第一行文本（空字符串则不显示）
     * @param text2 第二行文本（空字符串则不显示）
     */
    void UpdateOverlayText(const std::string& text1, const std::string& text2);

private:
    /** 服务器主循环：创建 socket、监听、接受连接 */
    void Run();

    /** 处理单个客户端连接，持续发送 MJPEG 流 */
    void HandleClient(int client_sock);

    int port_;          ///< 监听端口
    int width_;         ///< 发送图像宽度
    int height_;        ///< 发送图像高度
    int quality_;       ///< JPEG 质量

    std::thread server_thread_;         ///< 服务器线程
    std::atomic<bool> is_running_;      ///< 运行标志

    // 保护最新帧的互斥锁和条件变量
    std::mutex mutex_;
    std::condition_variable cv_;
    cv::Mat latest_frame_;
    bool has_new_frame_;

    int server_sock_;   ///< 监听 socket 文件描述符

    // ===== 新增 FPS 统计变量 =====
    int fps_frame_count_;                         // 帧计数
    double fps_;                                  // 当前 FPS 值
    std::chrono::steady_clock::time_point fps_last_time_; // 上次统计时间

    // 叠加文本相关
    std::string overlay_text1_;
    std::string overlay_text2_;
    mutable std::mutex text_mutex_;  // 保护上述两个字符串
};

#endif // HTTP_STREAM_THREAD_HPP