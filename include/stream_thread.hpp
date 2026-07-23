#ifndef STREAM_THREAD_HPP
#define STREAM_THREAD_HPP

#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <string>

// 包含你的日志头文件
#include "logger.hpp" 

class StreamThread {
public:
    StreamThread(const std::string& target_ip, int target_port, int width, int height, int quality);
    ~StreamThread();

    void Start();
    void Stop();
    
    // 主线程调用这个接口塞入新画面
    void UpdateFrame(const cv::Mat& frame);

private:
    void Run();

    std::string target_ip_;
    int target_port_;
    int sockfd_;

    std::thread thread_;
    std::atomic<bool> is_running_;
    
    std::mutex mutex_;
    std::condition_variable cv_;
    
    cv::Mat latest_frame_;
    bool has_new_frame_;

    // 从 config 读取的参数
    int width_;
    int height_;
    int quality_;
};

#endif // STREAM_THREAD_HPP