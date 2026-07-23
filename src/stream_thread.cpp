#include "stream_thread.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

StreamThread::StreamThread(const std::string& target_ip, int target_port, int width, int height, int quality)
    : target_ip_(target_ip), target_port_(target_port), width_(width), height_(height), quality_(quality), is_running_(false), has_new_frame_(false)
{
    // 1. 创建 UDP Socket
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) 
    {
        LOG_ERROR("[Stream] UDP Socket 创建失败！");
    }

    // 2. 开启广播权限 (允许发送到 10.42.0.255)
    int broadcast_enable = 1;
    setsockopt(sockfd_, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
}

StreamThread::~StreamThread() 
{
    Stop();

    if (sockfd_ >= 0) 
    {
        close(sockfd_);
    }
}

void StreamThread::Start() 
{
    if (!is_running_) 
    {
        is_running_ = true;
        thread_ = std::thread(&StreamThread::Run, this);
        LOG_INFO("[Stream] 图传线程已启动，目标 {}:{}", target_ip_, target_port_);
    }
}

void StreamThread::Stop() 

{
    if (is_running_) 
    {
        is_running_ = false;
        cv_.notify_all(); // 唤醒可能在休眠的线程

        if (thread_.joinable()) 
        {
            thread_.join();
        }
        LOG_INFO("[Stream] 图传线程已安全停止。");
    }
}

// 【关键设计】：主线程调用的更新函数
void StreamThread::UpdateFrame(const cv::Mat& frame) 

{
    if (!is_running_) return;

    // 使用 try_lock！如果图传线程正在压缩上一帧（持有了锁），
    // 这里的 try_lock 会返回 false，主线程直接跳过，绝不阻塞！
    if (mutex_.try_lock()) 
    {
        frame.copyTo(latest_frame_);
        has_new_frame_ = true;
        mutex_.unlock();
        cv_.notify_one(); // 通知图传线程：有新图了！
    }
}

void StreamThread::Run() 
{
    // 设置 UDP 目标地址信息
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(target_port_);
    dest_addr.sin_addr.s_addr = inet_addr(target_ip_.c_str());

    // 动态压缩参数
    std::vector<int> encode_params;
    encode_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    encode_params.push_back(quality_); 
    cv::Mat local_frame;
    std::vector<uchar> encode_buffer;

    while (is_running_) 
    {
        {
            // 等待新图像到来，或者超时唤醒

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() 
            { 
                return has_new_frame_ || !is_running_; 
            });

            if (!is_running_) break;
            if (!has_new_frame_) continue;

            // 把图像拷贝到局部变量，迅速释放锁，让主线程可以继续塞入下一帧
            latest_frame_.copyTo(local_frame);
            has_new_frame_ = false;
        }

        // ================== 在锁外进行耗时的图像处理 ==================

        if (local_frame.empty()) continue;

        try 
        {
            // 使用 config 里配置的宽高
            cv::Mat resized_frame;

            cv::resize(local_frame, resized_frame, cv::Size(width_, height_));

            // 2. 压缩为 JPEG 字节流
            cv::imencode(".jpg", resized_frame, encode_buffer, encode_params);

            // 3. 安全检查与发送 (UDP 单包极限约 65000 字节)
            if (encode_buffer.size() < 60000) 
            {
                sendto(sockfd_, encode_buffer.data(), encode_buffer.size(), 0,
                    (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            } 

            else 
            {
                LOG_WARN("[Stream] 单帧过大 ({} bytes)，丢弃发送", encode_buffer.size());
            }
        } 

        catch (const cv::Exception& e) 
        {
            LOG_ERROR("[Stream] 图像压缩失败: {}", e.what());
        }
    }
}

