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

    // 如果检测到了二维码，存储信息，并进行可视化绘制
    if (!decoded_info.empty())
    {
        restoreInfos(frame, decoded_info); 

        if (draw_result)
        {
            drawResults(frame, points, decoded_info);
        }

        return true;
    }

    return false;
}

void QRCodeScanner::restoreInfos(cv::Mat& frame, const std::vector<std::string>& decoded_info)
{ 
    // 存储目前检测的信息到历史记录 infos_ 中
    // 使用 std::find 查找目前检测到的二维码信息是否已经在历史记录中
    for (size_t i = 0; i < decoded_info.size(); i++)
    {
        // 获取当前二维码的文本内容
        std::string info = decoded_info[i];

        if (info.empty()) 
        {
            continue;
        }

        auto it = std::find(infos_.begin(), infos_.end(), info);

        // 判断迭代器是否等于 end()，如果等于说明没找到，也就是没记录过，这是新的二维码！！那就记录
        if (it == infos_.end()) 
        {
            infos_.push_back(info);
            std::cout << "[DETECTED] QRCode " << infos_.size() << ": " << info << std::endl;
        } 
    }
    
    // 检查是否已经记录到 4 个二维码
    if (infos_.size() == 4)
    {
        
        // todo: 下面可以写拼接组合的代码
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
    if (infos_.size() < 4)
    {
        printinfo = "Detected num: " + std::to_string(infos_.size());
    }
    else
    {
        printinfo = "Detected finished";
    }
    cv::putText(frame, printinfo, printinfo_position, cv::FONT_HERSHEY_SIMPLEX, 
                    1.5, green, 2, cv::LINE_AA);
}


std::vector<std::string>& QRCodeScanner::getInfos()
{
    return infos_;
}