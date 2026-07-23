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
    bool InitModel(const std::string& detect_prototxt,
                   const std::string& detect_caffe,
                   const std::string& sr_prototxt,
                   const std::string& sr_caffe);

    // 处理单帧图像，返回是否成功识别到至少一个二维码，draw_result参数决定是否要在原图上进行渲染
    bool ProcessFrame(cv::Mat& frame, bool draw_result = true);

    // 提供外部获取历史记录的接口 
    std::pair<uint8_t, uint8_t> GetResult();

    // 存储获得的两个带有有效信息的字符串
    std::string valid_info1_ = "";
    std::string valid_info2_ = "";

    std::string GetValidInfo1();
    std::string GetValidInfo2();

private:
    // 微信二维码检测器智能指针
    cv::Ptr<cv::wechat_qrcode::WeChatQRCode> detector;

    // 绘制识别结果（橙色边框与文字）
    void DrawResults(cv::Mat& frame, 
                     const std::vector<cv::Mat>& points, 
                     const std::vector<std::string>& decoded_info);

    // 解码目前检测的信息，并检测目前是否已经解码信息，在左上角提示
    void DecodeColorInfos(cv::Mat& frame, const std::vector<std::string>& decoded_info);

    // 存储目标 uint8_t 信息
    std::pair<uint8_t, uint8_t> color_infos_ = std::make_pair(0xFF, 0xFF);

};

#endif // QRCODE_SCANNER_HPP