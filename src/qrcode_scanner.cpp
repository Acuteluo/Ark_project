#include "../include/qrcode_scanner.hpp"

QRCodeScanner::QRCodeScanner()
{
    // 构造函数留空，初始化工作交由 initModel 完成
}

QRCodeScanner::~QRCodeScanner() 
{
    // 智能指针会自动释放内存，无需手动 delete
}

bool QRCodeScanner::initModel(const std::string& detect_prototxt,
                              const std::string& detect_caffe,
                              const std::string& sr_prototxt,
                              const std::string& sr_caffe)
{
    try
    {
        // 实例化微信二维码检测器
        detector = cv::makePtr<cv::wechat_qrcode::WeChatQRCode>(
            detect_prototxt, detect_caffe, sr_prototxt, sr_caffe);
        
        std::cout << "[qrcode_scanner.cpp] 微信二维码模型加载成功" << std::endl;
        return true;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[qrcode_scanner.cpp] 模型加载失败, OpenCV 报错: " << e.what() << std::endl;
        return false;
    }
}

bool QRCodeScanner::processFrame(cv::Mat& frame, bool draw_result)
{
    if (detector.empty())
    {
        std::cerr << "[qrcode_scanner.cpp] 检测器未初始化！" << std::endl;
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
        decodeColorInfos(frame, decoded_info); 

        if (draw_result)
        {
            drawResults(frame, points, decoded_info);
        }

        return true;
    }

    return false;
}

void QRCodeScanner::decodeColorInfos(cv::Mat& frame, const std::vector<std::string>& decoded_info)
{ 
    int color_count = 0;
    int basic_color = 0, core_color = 0;
    int basic_color_position = -1, core_color_position = -1;

    // 使用 std::find 查找目前检测到的二维码信息是否存有有效的信息
    for (size_t i = 0; i < decoded_info.size(); i++)
    {
        std::string info = decoded_info[i]; // 获取当前二维码的文本内容
        color_count = 0; // 颜色计数清零
        basic_color = 0, core_color = 0;
        basic_color_position = -1, core_color_position = -1;

        if (info.empty()) 
        {
            continue;
        }

        for (int j = 1; j < 4; j++)
        {
            if (info.find(colors[j]) != std::string::npos)
            {
                color_count++;
                if (color_count == 1)
                {
                    basic_color = j;
                    basic_color_position = info.find(colors[j]);
                }
                else
                {
                    core_color = j;
                    core_color_position = info.find(colors[j]);
                }
            }
        }

        // 集齐两种颜色，而且没有存储过
        if (color_count == 2 && ultimately_basic_color_ == 0x00 && ultimately_core_color_ == 0x00)
        {
            if (basic_color_position > core_color_position)
            {
                std::swap(basic_color, core_color); // 基础颜色在前，核心颜色在后
            }

            ultimately_basic_color_ = basic_color; // 存储基础颜色
            ultimately_core_color_ = core_color; // 存储核心颜色

            std::cout << "[qrcode_scanner.cpp] 检测到基础颜色: " << colors[basic_color] 
                      << ", 核心颜色: " << colors[core_color] << std::endl;
        }
        
    }

}

void QRCodeScanner::drawResults(cv::Mat& frame, 
                                const std::vector<cv::Mat>& points, 
                                const std::vector<std::string>& decoded_info)
{
    // 定义橙色
    cv::Scalar orange(0, 165, 255);
    cv::Scalar green(0, 255, 0);

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
        
        // 在终端同步输出信息，方便调试
        // std::cout << "[DETECTED] QRCode " << i + 1 << ": " << info << std::endl;
    }

    // 在图上显示目前检测到的二维码数量
    cv::Point2f printinfo_position(10, 50);
    std::string printinfo;
    if (ultimately_basic_color_ == 0x00 || ultimately_core_color_ == 0x00)
    {
        printinfo = "UNDETECTED";
    }
    else
    {
        printinfo = "DETECTED: basic=" + colors[ultimately_basic_color_] + " core=" + colors[ultimately_core_color_];
    }
    cv::putText(frame, printinfo, printinfo_position, cv::FONT_HERSHEY_SIMPLEX, 
                    1.5, green, 2, cv::LINE_AA);
}


std::pair<uint8_t, uint8_t> QRCodeScanner::getuint8t()
{
    return std::make_pair(ultimately_basic_color_, ultimately_core_color_);
}