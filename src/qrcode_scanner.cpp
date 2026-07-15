#include "qrcode_scanner.hpp"
#include "color.hpp"

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
        std::cout << RED << "[qrcode_scanner.cpp] 模型加载失败, OpenCV 报错: " << e.what() << NONE << std::endl;
        return false;
    }
}

bool QRCodeScanner::processFrame(cv::Mat& frame, bool draw_result)
{
    if (detector.empty())
    {
        std::cout << RED << "[qrcode_scanner.cpp] 检测器未初始化！" << NONE << std::endl;
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
    int color_count = 0; // 颜色计数，1=识别篮筐，2=基础球+核心球
    int basic_color = 0xFF, core_color = 0xFF;
    int basic_color_position = -1, core_color_position = -1;
    bool is_comma = false; // 用于判断是否有逗号分隔符

    // 使用 std::find 查找目前检测到的二维码信息是否存有有效的信息
    for (size_t i = 0; i < decoded_info.size(); i++)
    {
        std::string info = decoded_info[i]; // 获取当前二维码的文本内容
        color_count = 0; // 颜色计数清零
        basic_color = 0xFF, core_color = 0xFF;
        basic_color_position = -1, core_color_position = -1;
        is_comma = false;

        // 跳过空字符串
        if (info.empty()) 
        {
            continue;
        }

        // 检查是否包含逗号分隔符
        is_comma = (info.find(',') != std::string::npos);

        // 遍历颜色映射表（"" Red Yellow Blue），查找当前二维码信息中是否包含已知颜色
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

        // 一个二维码里集齐两种颜色，并且有,分隔符，确保都不是0xFF，说明是识别到 基础颜色 和 核心颜色 了
        if (color_count == 2 && is_comma && (basic_color != 0xFF && core_color != 0xFF))
        {
            if (basic_color_position == -1 || core_color_position == -1)
            {
                std::cout << RED << "[qrcode_scanner.cpp] 错误: 颜色位置为 -1" << NONE << std::endl;
                continue;
            }

            // 确保基础颜色在前，核心颜色在后
            if (basic_color_position > core_color_position)
            {
                std::swap(basic_color, core_color); 
                std::swap(basic_color_position, core_color_position);
            }

            // 只有有效信息不同的时候才改变存储的信息
            if (basic_color != color_infos_.first || core_color != color_infos_.second)
            {
                color_infos_.first = basic_color; // 存储基础颜色
                color_infos_.second = core_color; // 存储核心颜色

                std::cout << "[qrcode_scanner.cpp] 【有效信息更新】检测到基础颜色: " << GREEN << colors[basic_color] 
                          << NONE << ", 核心颜色: " << GREEN << colors[core_color] << NONE << std::endl;
            }
        }
        
        // 一个二维码里集齐一种颜色，并且没有,分隔符，说明是识别到 球框颜色 了
        else if (color_count == 1 && !is_comma && (basic_color != 0xFF && core_color == 0xFF))
        { 
            if (basic_color_position == -1 || core_color_position != -1)
            {
                std::cout << RED << "[qrcode_scanner.cpp] 错误: 颜色位置为 -1 或 颜色存储错误" << NONE << std::endl;
                continue;
            }

            std::swap(basic_color, core_color);
            std::swap(basic_color_position, core_color_position);

            if (basic_color != color_infos_.first || core_color != color_infos_.second)
            {
                color_infos_.first = basic_color; // 存储 0xFF
                color_infos_.second = core_color; // 存储 球框颜色

                std::cout << "[qrcode_scanner.cpp] 【有效信息更新】检测到球框颜色: " << GREEN << colors[core_color] << NONE << std::endl;
            }
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
    if (color_infos_.first == 0xFF && color_infos_.second == 0xFF)
    {
        printinfo = "NO INFO";
    }
    else
    {
        if (color_infos_.first == 0xFF) 
        {
            printinfo = "INFO: color1=NULL color2=" + colors[color_infos_.second];
        }
        else if (color_infos_.second == 0xFF) 
        {
            printinfo = "INFO: color1=" + colors[color_infos_.first] + " color2=NULL";
        }
        else
        {
            printinfo = "INFO: color1=" + colors[color_infos_.first] + " color2=" + colors[color_infos_.second];
        }
    }

    cv::putText(frame, printinfo, printinfo_position, cv::FONT_HERSHEY_SIMPLEX, 
                    1.5, green, 2, cv::LINE_AA);
}


std::pair<uint8_t, uint8_t> QRCodeScanner::getuint8t()
{
    return color_infos_;
}