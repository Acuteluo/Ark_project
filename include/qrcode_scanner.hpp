#ifndef QRCODE_SCANNER_HPP
#define QRCODE_SCANNER_HPP

#include <opencv2/opencv.hpp>
#include <opencv2/wechat_qrcode.hpp>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

class QRCodeScanner
{
public:
    // 构造函数
    QRCodeScanner();

    // 析构函数
    ~QRCodeScanner();

    // 初始化模型，传入模型文件路径
    bool initModel(const std::string& detect_prototxt,
                   const std::string& detect_caffe,
                   const std::string& sr_prototxt,
                   const std::string& sr_caffe);

    // 处理单帧图像，返回是否成功识别到至少一个二维码
    bool processFrame(cv::Mat& frame);

    // 【新增】提供外部获取历史记录的接口 
    std::vector<std::string>& getInfos();

private:
    // 微信二维码检测器智能指针
    cv::Ptr<cv::wechat_qrcode::WeChatQRCode> detector;

    // 绘制识别结果（橙色边框与文字）
    void drawResults(cv::Mat& frame, 
                     const std::vector<cv::Mat>& points, 
                     const std::vector<std::string>& decoded_info);

    // 存储目前检测的信息，并检测目前是否已经检测够 4 个二维码信息，在左上角提示
    void restoreInfos(cv::Mat& frame, const std::vector<std::string>& decoded_info);

    // 存储历史识别结果
    std::vector<std::string> infos_;
};

#endif // QRCODE_SCANNER_HPP