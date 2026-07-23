#include "qrcode_scanner.hpp"
#include "logger.hpp"

QRCodeScanner::QRCodeScanner()
{
    // 构造函数留空，初始化工作交由 initModel 完成
}

QRCodeScanner::~QRCodeScanner() 
{
    // 智能指针会自动释放内存，无需手动 delete
}

bool QRCodeScanner::InitModel(const std::string& detect_prototxt,
                              const std::string& detect_caffe,
                              const std::string& sr_prototxt,
                              const std::string& sr_caffe)
{
    try
    {
        // 实例化微信二维码检测器
        detector = cv::makePtr<cv::wechat_qrcode::WeChatQRCode>(
            detect_prototxt, detect_caffe, sr_prototxt, sr_caffe);
        
        LOG_INFO("[qrcode_scanner.cpp] 微信模型加载成功");
        return true;
    }
    catch (const cv::Exception& e)
    {
        LOG_ERROR("[qrcode_scanner.cpp] 微信模型加载失败, OpenCV 报错：{}", e.what());
        return false;
    }
}

bool QRCodeScanner::ProcessFrame(cv::Mat& frame, bool draw_result)
{
    if (detector.empty())
    {
        LOG_ERROR("[qrcode_scanner.cpp] 检测器未初始化！");
        return false;
    }

    // 存储解码后的文本内容
    std::vector<std::string> decoded_info;
    // 存储每个二维码的四个顶点坐标
    std::vector<cv::Mat> points;

    // 执行检测与解码
    decoded_info = detector->detectAndDecode(frame, points);

    // 如果检测到了二维码，解码信息，并进行可视化绘制
    if (!decoded_info.empty())
    {
        DecodeColorInfos(frame, decoded_info); 
    }

    // 无论当前有没有检测到二维码，只要开启了显示模式，就把现有的历史记录贴在屏幕上。
    // 如果 decoded_info 为空，DrawResults 内部的橙色框 for 循环会自动跳过，只画文字。
    if (draw_result)
    {
        DrawResults(frame, points, decoded_info);
    }

    return !decoded_info.empty();
}

void QRCodeScanner::DecodeColorInfos(cv::Mat& frame, const std::vector<std::string>& decoded_info)
{ 
    int red_position = -1, yellow_position = -1, blue_position = -1; // 用来找字符串中是否有颜色
    int color_count = 0; // 存在颜色的数量

    // 使用 std::find 查找目前检测到的二维码信息是否存有有效的信息
    for (size_t i = 0; i < decoded_info.size(); i++)
    {
        std::string info = decoded_info[i]; // 获取当前二维码的文本内容

        // 跳过空字符串
        if (info.empty()) 
        {
            continue;
        }

        // 先清空数据，然后查找三种颜色是否都出现过
        red_position = -1;
        yellow_position = -1;
        blue_position = -1;
        color_count = 0;

        red_position = info.find("Red");
        yellow_position = info.find("Yellow");
        blue_position = info.find("Blue");
        color_count = (red_position != -1) + (yellow_position != -1) + (blue_position != -1);

        // 假如识别到 RESET!!! 就全部重置
        if (info == "RESET!!!")
        {
            color_infos_.first = 0xFF;
            color_infos_.second = 0xFF;
            valid_info1_ = "";
            valid_info2_ = "";
            LOG_INFO("[qrcode_scanner.cpp] 检测到 RESET!!! ，已重置");
            break; // 不再检测后续的二维码
        }

        // 开始大段判断，代码长是长，但是逻辑最清楚最简单
        if (color_count <= 1)
        {
            continue;
        }
        else if (color_count == 2) // 存在两种颜色，那就是 基础框 和 核心框 对应的颜色
        { 
            uint8_t data1 = 0xFF; // 有效信息 1

            if (red_position == -1) // 存在蓝黄
            {
                if (yellow_position < blue_position) data1 = 0x04; // 0x04：Yellow,Blue
                else data1 = 0x06;                                 // 0x06：Blue,Yellow
            }
            else if (yellow_position == -1) // 存在红蓝
            {
                if (red_position < blue_position) data1 = 0x02;    // 0x02：Red,Blue
                else data1 = 0x05;                                 // 0x05：Blue,Red
            }
            else // 存在红黄
            {
                if (red_position < yellow_position) data1 = 0x01;  // 0x01：Red,Yellow
                else data1 = 0x03;                                 // 0x03：Yellow,Red
            }

            // 只有有效信息不同的时候才改变存储的信息
            if (data1 != 0xFF && data1 != color_infos_.first)
            {
                color_infos_.first = data1; // 存储 基础框、核心框 对应的颜色
                valid_info1_ = info;
                LOG_INFO("[qrcode_scanner.cpp] data1 更新，检测到 基础框 和 核心框 的颜色: {}", valid_info1_);
            }
        }
        else if (color_count == 3) // 存在三种颜色，那就是机器人面朝着的 投球框 对应的颜色顺序
        {
            uint8_t data2 = 0xFF; // 有效信息 2

            if (red_position < yellow_position && yellow_position < blue_position)      // 0x01：Red,Yellow,Blue
            {
                data2 = 0x01; 
            }
            else if (red_position < blue_position && blue_position < yellow_position)   // 0x02：Red,Blue,Yellow
            {
                data2 = 0x02; 
            }
            else if (yellow_position < red_position && red_position < blue_position)    // 0x03：Yellow,Red,Blue
            {
                data2 = 0x03; 
            }
            else if(yellow_position < blue_position && blue_position < red_position)    // 0x04：Yellow,Blue,Red
            {
                data2 = 0x04; 
            }
            else if (blue_position < red_position && red_position < yellow_position)    // 0x05：Blue,Red,Yellow
            {
                data2 = 0x05; 
            }
            else if (blue_position < yellow_position && yellow_position < red_position) // 0x06：Blue,Yellow,Red
            {
                data2 = 0x06; 
            }

            if (data2 != 0xFF && data2 != color_infos_.second)
            {
                color_infos_.second = data2; // 存储 三个投球框 对应的颜色顺序
                valid_info2_ = info;
                LOG_INFO("[qrcode_scanner.cpp] data2 更新，检测到 三个投球框 的颜色顺序: {}", valid_info2_);
            }
        }
        
    }

}

void QRCodeScanner::DrawResults(cv::Mat& frame, 
                                const std::vector<cv::Mat>& points, 
                                const std::vector<std::string>& decoded_info)
{
    // 定义橙色
    cv::Scalar orange(0, 165, 255);
    cv::Scalar green(0, 255, 0);
    cv::Scalar red(0, 0, 255);

    // 遍历所有检测到的二维码，画橙色框和打印信息
    for (size_t i = 0; i < decoded_info.size(); i++)
    {
        // 获取当前二维码的文本内容
        std::string info = decoded_info[i];

        // 如果内容为空，跳过绘制
        if (info.empty())
        {
            continue;
        }

        // points[i] 包含了该二维码的四个顶点 (左上，右上，右下，左下)
        // 数据类型通常为 CV_32FC2
        cv::Mat pt_mat = points[i];
        
        // 提取四个顶点
        cv::Point2f pt1(pt_mat.at<float>(0, 0), pt_mat.at<float>(0, 1));
        cv::Point2f pt2(pt_mat.at<float>(1, 0), pt_mat.at<float>(1, 1));
        cv::Point2f pt3(pt_mat.at<float>(2, 0), pt_mat.at<float>(2, 1));
        cv::Point2f pt4(pt_mat.at<float>(3, 0), pt_mat.at<float>(3, 1));

        // 用橙色线条将四个顶点连接起来，形成轮廓框
        int thickness = 5;
        cv::line(frame, pt1, pt2, orange, thickness);
        cv::line(frame, pt2, pt3, orange, thickness);
        cv::line(frame, pt3, pt4, orange, thickness);
        cv::line(frame, pt4, pt1, orange, thickness);

        // 将解码内容打印在二维码的左上角附近
        // 为了防止文字与边框重叠，Y轴向上偏移 10 个像素
        cv::Point2f text_origin(pt1.x, pt1.y - 10);
        
        cv::putText(frame, info, text_origin, cv::FONT_HERSHEY_SIMPLEX, 
                    1.5, green, 2, cv::LINE_AA);
        
    }

    // // 在图上显示目前检测到的二维码数量
    // cv::Point2f printinfo1_position(20, 50);
    // cv::Point2f printinfo2_position(20, 100);
    // std::string printinfo1 = "INFO 1 = NO INFO";
    // std::string printinfo2 = "INFO 2 = NO INFO";

    // if (color_infos_.first != 0xFF)
    // {
    //     printinfo1 = "INFO 1 = " + valid_info1_;
    // }

    // if (color_infos_.second != 0xFF)
    // {
    //     printinfo2 = "INFO 2 = " + valid_info2_;
    // }
    
    // cv::putText(frame, printinfo1, printinfo1_position, cv::FONT_HERSHEY_SIMPLEX, 
    //                 1.5, red, 2, cv::LINE_AA);
    // cv::putText(frame, printinfo2, printinfo2_position, cv::FONT_HERSHEY_SIMPLEX, 
    //                 1.5, red, 2, cv::LINE_AA);
}


std::pair<uint8_t, uint8_t> QRCodeScanner::GetResult()
{
    return color_infos_;
}


std::string QRCodeScanner::GetValidInfo1()
{ 
    return "INFO 1 = " + valid_info1_; 
}

std::string QRCodeScanner::GetValidInfo2()

{ 
    return "INFO 2 = " + valid_info2_; 
}