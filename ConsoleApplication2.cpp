#include <iostream>
#include <ins_realtime_stitcher.h>
#include <camera/camera.h>
#include <camera/device_discovery.h>

#include <iostream>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include <opencv2/opencv.hpp>
#include <queue>
#include <atomic>

// Windows 兼容性头文件
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#define ACCESS_FUNC _access
#define F_OK 0
#else
#include <unistd.h>
#define ACCESS_FUNC access
#define F_OK 0
#endif

#pragma comment(lib, "ws2_32.lib")

const std::string window_name = "realtime_stitcher";

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);

    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

class VideoStreamer {
public:
    VideoStreamer(const std::string& serverIp, int videoPort, int audioPort)
        : serverIp_(serverIp), videoPort_(videoPort), audioPort_(audioPort), 
          videoConnected_(false), audioConnected_(false) {
    }

    ~VideoStreamer() { 
        DisconnectVideo(); 
        DisconnectAudio();
    }

    bool ConnectVideo() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cout << "[VIDEO_ERROR] WSAStartup失败" << std::endl;
            return false;
        }
        videoSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (videoSock_ == INVALID_SOCKET) { 
            std::cout << "[VIDEO_ERROR] 创建socket失败" << std::endl;
            WSACleanup(); 
            return false; 
        }
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(videoPort_);
        inet_pton(AF_INET, serverIp_.c_str(), &serverAddr.sin_addr);
        if (connect(videoSock_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cout << "[VIDEO_ERROR] 连接到视频服务器失败，端口: " << videoPort_ << std::endl;
            closesocket(videoSock_); 
            return false;
        }
        videoConnected_ = true;
        std::cout << "[VIDEO_SUCCESS] 视频连接建立成功" << std::endl;
        return true;
    }

    bool ConnectAudio() {
        // 检查WinSock是否已初始化，如果没有则初始化
        audioSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (audioSock_ == INVALID_SOCKET) {
            // 如果socket创建失败，尝试初始化WinSock
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                std::cout << "[AUDIO_ERROR] WSAStartup失败" << std::endl;
                return false;
            }
            audioSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (audioSock_ == INVALID_SOCKET) {
                std::cout << "[AUDIO_ERROR] 创建socket失败" << std::endl;
                return false;
            }
        }
        
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(audioPort_);
        inet_pton(AF_INET, serverIp_.c_str(), &serverAddr.sin_addr);
        
        std::cout << "[AUDIO_INFO] 尝试连接到音频服务器 " << serverIp_ << ":" << audioPort_ << std::endl;
        
        if (connect(audioSock_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cout << "[AUDIO_ERROR] 连接到音频服务器失败，端口: " << audioPort_ 
                     << ", 错误码: " << error << std::endl;
            closesocket(audioSock_); 
            return false;
        }
        audioConnected_ = true;
        std::cout << "[AUDIO_SUCCESS] 音频连接建立成功" << std::endl;
        return true;
    }

    void DisconnectVideo() {
        if (videoConnected_) {
            videoConnected_ = false;
            closesocket(videoSock_);
        }
    }

    void DisconnectAudio() {
        if (audioConnected_) {
            audioConnected_ = false;
            closesocket(audioSock_);
        }
    }

    bool IsVideoConnected() const { return videoConnected_; }
    bool IsAudioConnected() const { return audioConnected_; }

    void SendVideoFrame(const uint8_t* data, size_t size, int64_t timestamp, uint16_t width, uint16_t height) {
        if (!videoConnected_) return;
        
        // 强制结构体按1字节对齐，避免内存填充
        #pragma pack(push, 1)
        struct FrameHeader {
            uint32_t magic;
            uint32_t headerSize;
            uint32_t payloadSize;
            uint16_t width;
            uint16_t height;
            int64_t timestamp;
        } header;
        #pragma pack(pop)
        
        header.magic = 0x12345678;
        header.headerSize = sizeof(FrameHeader);
        header.payloadSize = static_cast<uint32_t>(size);
        header.width = width;
        header.height = height;
        header.timestamp = timestamp;
        send(videoSock_, reinterpret_cast<const char*>(&header), sizeof(header), 0);
        const char* ptr = reinterpret_cast<const char*>(data);
        size_t remaining = size;
        while (remaining > 0) {
            int bytesSent = send(videoSock_, ptr, static_cast<int>(remaining), 0);
            if (bytesSent <= 0) break;
            remaining -= bytesSent;
            ptr += bytesSent;
        }
    }
    
    void SendAudioData(const uint8_t* data, size_t size, int64_t timestamp) {
        if (!audioConnected_ || !data || size == 0) return;
        
        // 强制结构体按1字节对齐，避免内存填充
        #pragma pack(push, 1)
        struct AudioHeader {
            uint32_t magic;
            uint32_t headerSize;
            uint32_t payloadSize;
            int64_t timestamp;
        } header;
        #pragma pack(pop)
        
        header.magic = 0x87654321;
        header.headerSize = sizeof(AudioHeader);
        header.payloadSize = static_cast<uint32_t>(size);
        header.timestamp = timestamp;
        
        // 调试：输出结构体大小和内容
        static int debug_count = 0;
        if (debug_count < 3) {
            std::cout << "[AUDIO_DEBUG] 音频头结构体大小: " << sizeof(AudioHeader) << " 字节" << std::endl;
            std::cout << "[AUDIO_DEBUG] 发送: magic=0x" << std::hex << header.magic 
                     << ", size=" << std::dec << header.payloadSize 
                     << ", headerSize=" << header.headerSize 
                     << ", timestamp=" << header.timestamp << std::endl;
            debug_count++;
        }
        
        // 发送音频头部
        int headerSent = send(audioSock_, reinterpret_cast<const char*>(&header), sizeof(header), 0);
        if (headerSent <= 0) {
            std::cout << "[AUDIO_ERROR] 发送音频头部失败，连接可能断开" << std::endl;
            audioConnected_ = false;
            return;
        }
        
        // 发送音频数据
        const char* ptr = reinterpret_cast<const char*>(data);
        size_t remaining = size;
        while (remaining > 0) {
            int bytesSent = send(audioSock_, ptr, static_cast<int>(remaining), 0);
            if (bytesSent <= 0) {
                std::cout << "[AUDIO_ERROR] 发送音频数据失败" << std::endl;
                audioConnected_ = false;
                return;
            }
            remaining -= bytesSent;
            ptr += bytesSent;
        }
        
        // 统计音频包发送
        static int audio_packet_count = 0;
        audio_packet_count++;
        
        if (audio_packet_count % 50 == 1) {
            std::cout << "[AUDIO] 发送音频包: " << audio_packet_count 
                     << ", 大小: " << size << " 字节" << std::endl;
        }
    }

private:
    std::string serverIp_;
    int videoPort_;
    int audioPort_;
    SOCKET videoSock_;
    SOCKET audioSock_;
    bool videoConnected_;
    bool audioConnected_;
};

class StitchDelegate : public ins_camera::StreamDelegate {
public:
    StitchDelegate(const std::shared_ptr<ins::RealTimeStitcher>& stitcher, VideoStreamer* streamer) 
        : stitcher_(stitcher), streamer_(streamer), streaming_active_(false) {
    }

    virtual ~StitchDelegate() {
    }
    
    void SetStreamingActive(bool active) {
        streaming_active_ = active;
        if (active) {
            std::cout << "[INFO] 音频处理已启用" << std::endl;
        } else {
            std::cout << "[INFO] 音频处理已停止" << std::endl;
        }
    }

    void OnAudioData(const uint8_t* data, size_t size, int64_t timestamp) override {
        // 只有在推流激活时才处理音频
        if (!streaming_active_) {
            return;
        }
        
        // 发送音频数据到服务器
        if (streamer_ && streamer_->IsAudioConnected() && data && size > 0) {
            streamer_->SendAudioData(data, size, timestamp);
        }
        
        // 简单的音频统计
        static int audio_packet_count = 0;
        audio_packet_count++;
        
        if (audio_packet_count % 100 == 1) {
            std::cout << "[AUDIO] 接收音频包: " << audio_packet_count 
                     << ", 大小: " << size << " 字节" << std::endl;
        }
    }

    void OnVideoData(const uint8_t* data, size_t size, int64_t timestamp, uint8_t streamType, int stream_index) override {
        // Do not send H.264 compressed data to server here
        // The 'data' here is compressed H.264 packets, not raw image data
        
        // Continue processing for stitching
        stitcher_->HandleVideoData(data, size, timestamp, streamType, stream_index);
    }

    void OnGyroData(const std::vector<ins_camera::GyroData>& data) override {
        std::vector<ins::GyroData> data_vec(data.size());
        memcpy(data_vec.data(), data.data(), data.size() * sizeof(ins_camera::GyroData));
        stitcher_->HandleGyroData(data_vec);
    }

    void OnExposureData(const ins_camera::ExposureData& data) override {
        ins::ExposureData exposure_data{};
        exposure_data.exposure_time = data.exposure_time;
        exposure_data.timestamp = data.timestamp;
        stitcher_->HandleExposureData(exposure_data);
    }

private:
    std::shared_ptr<ins::RealTimeStitcher> stitcher_;
    VideoStreamer* streamer_;
    bool streaming_active_;
};

// 添加多线程处理相关的类
class FrameProcessor {
private:
    struct FrameData {
        cv::Mat frame;
        int64_t timestamp;
        uint16_t width;
        uint16_t height;
    };

    std::queue<FrameData> frameQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::atomic<bool> shouldStop{false};
    std::thread processingThread;
    VideoStreamer* streamer;

public:
    FrameProcessor(VideoStreamer* streamer) : streamer(streamer) {
        // 启动处理线程
        processingThread = std::thread(&FrameProcessor::ProcessFrames, this);
    }

    ~FrameProcessor() {
        shouldStop.store(true);
        queueCondition.notify_all();
        if (processingThread.joinable()) {
            processingThread.join();
        }
    }

    void AddFrame(const cv::Mat& frame, int64_t timestamp, uint16_t width, uint16_t height) {
        std::lock_guard<std::mutex> lock(queueMutex);
        
        // 限制队列大小，防止内存积累
        while (frameQueue.size() >= 3) {
            frameQueue.pop();  // 丢弃最旧的帧
        }
        
        frameQueue.push({frame.clone(), timestamp, width, height});
        queueCondition.notify_one();
    }

private:
    void ProcessFrames() {
        while (!shouldStop.load()) {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCondition.wait(lock, [this] { 
                return !frameQueue.empty() || shouldStop.load(); 
            });

            if (shouldStop.load()) break;

            if (!frameQueue.empty()) {
                FrameData frameData = frameQueue.front();
                frameQueue.pop();
                lock.unlock();

                // 在后台线程中进行颜色转换和网络发送
                try {
                    cv::Mat rgb_frame;
                    cv::cvtColor(frameData.frame, rgb_frame, cv::COLOR_BGRA2RGB);
                    
                    if (streamer && streamer->IsVideoConnected()) {
                        size_t converted_size = rgb_frame.total() * rgb_frame.elemSize();
                        streamer->SendVideoFrame(rgb_frame.data, converted_size, 
                                               frameData.timestamp, frameData.width, frameData.height);
                    }
                } catch (...) {
                    // 忽略处理异常
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "[INFO] 程序启动，初始化环境..." << std::endl;
    ins::InitEnv();
    std::cout << "[INFO] 开始打开相机..." << std::endl;
    ins_camera::SetLogLevel(ins_camera::LogLevel::WARNING);
    ins::SetLogLevel(ins::InsLogLevel::WARNING);
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == std::string("--debug")) {
            ins_camera::SetLogLevel(ins_camera::LogLevel::VERBOSE);
            std::cout << "[INFO] 已启用调试日志等级" << std::endl;
        }
        else if (arg == std::string("--log_file")) {
            const std::string log_file = argv[++i];
            ins_camera::SetLogPath(log_file);
            std::cout << "[INFO] 日志文件路径: " << log_file << std::endl;
        }
    }

    // 初始化分离的音视频流推送
    std::cout << "[INFO] 尝试连接到服务器..." << std::endl;
    std::unique_ptr<VideoStreamer> streamer = std::make_unique<VideoStreamer>("49.233.204.88", 8888, 8890);
    
    if (!streamer->ConnectVideo()) {
        std::cerr << "[WARN] 无法连接到视频服务器，视频流不会发送！" << std::endl;
    } else {
        std::cout << "[INFO] 已连接到视频服务器(8888)，视频流将实时发送。" << std::endl;
    }
    
    if (!streamer->ConnectAudio()) {
        std::cerr << "[WARN] 无法连接到音频服务器，音频流不会发送！" << std::endl;
    } else {
        std::cout << "[INFO] 已连接到音频服务器(8890)，音频流将实时发送。" << std::endl;
    }

    std::cout << "[INFO] 正在查找可用相机设备..." << std::endl;
    ins_camera::DeviceDiscovery discovery;
    auto list = discovery.GetAvailableDevices();
    if (list.empty()) {
        std::cerr << "[ERROR] 未找到任何相机设备，程序即将退出。" << std::endl;
        discovery.FreeDeviceDescriptors(list);
        return -1;
    }
    std::cout << "[INFO] 发现 " << list.size() << " 个设备。" << std::endl;

    for (const auto& camera : list) {
        std::cout << "serial:" << camera.serial_number << "\t"
            << ";camera type:" << camera.camera_name << "\t"
            << ";fw version:" << camera.fw_version << "\t"
            << std::endl;
    }

    std::cout << "[INFO] 尝试打开第一个相机..." << std::endl;
    auto cam = std::make_shared<ins_camera::Camera>(list[0].info);
    if (!cam->Open()) {
        std::cerr << "[ERROR] 打开相机失败，程序即将退出。" << std::endl;
        return -1;
    }
    std::cout << "[INFO] 相机打开成功。" << std::endl;

    const auto serial_number = list[0].serial_number;

    discovery.FreeDeviceDescriptors(list);

    cv::Mat show_image_;
    cv::Mat backup_image_;
    std::atomic<bool> new_frame_ready_{false};
    bool is_stop_ = true;

    std::shared_ptr<ins::RealTimeStitcher> stitcher = std::make_shared<ins::RealTimeStitcher>();
    ins::CameraInfo camera_info;
    auto preview_param = cam->GetPreviewParam();
    camera_info.cameraName = preview_param.camera_name;
    camera_info.decode_type = static_cast<ins::VideoDecodeType>(preview_param.encode_type);
    camera_info.offset = preview_param.offset;
    auto window_crop_info = preview_param.crop_info;
    camera_info.window_crop_info_.crop_offset_x = window_crop_info.crop_offset_x;
    camera_info.window_crop_info_.crop_offset_y = window_crop_info.crop_offset_y;
    camera_info.window_crop_info_.dst_width = window_crop_info.dst_width;
    camera_info.window_crop_info_.dst_height = window_crop_info.dst_height;
    camera_info.window_crop_info_.src_width = window_crop_info.src_width;
    camera_info.window_crop_info_.src_height = window_crop_info.src_height;
    camera_info.gyro_timestamp = preview_param.gyro_timestamp;

    stitcher->SetCameraInfo(camera_info);
    stitcher->SetStitchType(ins::STITCH_TYPE::DYNAMICSTITCH);
    stitcher->EnableFlowState(true);
    stitcher->SetOutputSize(1440, 720);  // 使用720p输出
    stitcher->SetStitchRealTimeDataCallback([&](uint8_t* data[4], int linesize[4], int width, int height, int format, int64_t timestamp) {
        // This handles local display and server transmission
        if (!is_stop_ && data[0] != nullptr) {
            try {
                backup_image_ = cv::Mat(height, width, CV_8UC4, data[0]).clone();
                new_frame_ready_.store(true);
                
                // Send processed raw image data to server
                if (streamer && streamer->IsVideoConnected()) {
                    cv::Mat rgb_frame;
                    cv::cvtColor(backup_image_, rgb_frame, cv::COLOR_BGRA2RGB);
                    size_t converted_size = rgb_frame.total() * rgb_frame.elemSize();
                    streamer->SendVideoFrame(rgb_frame.data, converted_size, timestamp, width, height);
                    
                    // 统计发送到后端的帧率
                    static int sent_frame_count = 0;
                    static auto sent_start_time = std::chrono::steady_clock::now();
                    sent_frame_count++;
                    
                    auto current_time = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - sent_start_time).count();
                    if (elapsed >= 2 && sent_frame_count > 0) { // 每2秒输出一次
                        double sent_fps = sent_frame_count / (double)elapsed;
                        std::cout << "[SEND_INFO] 发送到后端帧率: " << sent_fps << " FPS" << std::endl;
                        sent_frame_count = 0;
                        sent_start_time = current_time;
                    }
                }
            } catch (...) {
                // Ignore exceptions
            }
        }
    });

    auto delegate = std::make_shared<StitchDelegate>(stitcher, streamer.get());
    std::shared_ptr<ins_camera::StreamDelegate> base_delegate = delegate;
    cam->SetStreamDelegate(base_delegate);

    std::cout << "[INFO] 相机初始化完成，进入主菜单。" << std::endl;

    std::cout << "Usage:" << std::endl;
    std::cout << "1: start preview live streaming:" << std::endl;
    std::cout << "2: stop preview live streaming:" << std::endl;

    int option = 0;
    while (true) {
        std::cout << "\n[INPUT] 请输入操作编号 (1: 开始推流, 2: 停止推流, 0: 退出): ";
        std::cin >> option;
        if (option < 0 || option > 39) {
            std::cout << "[WARN] 输入无效，请重新输入。" << std::endl;
            continue;
        }

        if (option == 0) {
            std::cout << "[INFO] 用户选择退出程序。" << std::endl;
            break;
        }

        if (option == 1) {
            if (!is_stop_) {
                std::cout << "[WARN] 推流已在进行中。" << std::endl;
                continue;
            }
            std::cout << "[INFO] 正在启动推流..." << std::endl;
            
            // 设置视频参数
            ins_camera::LiveStreamParam param;
            param.video_resolution = ins_camera::VideoResolution::RES_1440_720P30;
            param.lrv_video_resulution = ins_camera::VideoResolution::RES_1440_720P30;
            param.video_bitrate = 1024 * 1024 * 10; // 10Mbps
            param.enable_audio = true;  // 确保启用音频传输
            param.audio_samplerate = 48000;  // 48kHz采样率
            param.audio_bitrate = 128000;    // 128kbps音频码率
            param.using_lrv = false;
            
            // 检查摄像头状态
            if (!cam) {
                std::cerr << "[ERROR] 摄像头未初始化。" << std::endl;
                continue;
            }
            
            if (cam->StartLiveStreaming(param)) {
                // 添加延迟确保摄像头完全启动
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                
                stitcher->StartStitch();
                delegate->SetStreamingActive(true);  // 启用音频处理
                std::cout << "[INFO] 成功启动推流。" << std::endl;
                is_stop_ = false;
                
                // 创建可调整大小的窗口
                cv::namedWindow(window_name, cv::WINDOW_NORMAL);
                cv::resizeWindow(window_name, 1440, 720);
                
                std::cout << "[INFO] 视频窗口已打开，按 'q' 或 'ESC' 键退出视频显示并返回菜单" << std::endl;
                std::cout << "[INFO] 音频和视频传输已启用..." << std::endl;
                
                int frame_count = 0;
                auto start_time = std::chrono::steady_clock::now();
                
                // 视频显示循环
                while (!is_stop_) {
                    // 检查是否有新帧
                    if (new_frame_ready_.load()) {
                        try {
                            if (!backup_image_.empty()) {
                                show_image_ = backup_image_.clone();
                                new_frame_ready_.store(false);
                                
                                cv::cvtColor(show_image_, show_image_, cv::COLOR_RGBA2BGRA);
                                cv::imshow(window_name, show_image_);
                                frame_count++;
                                
                                // 每3秒输出一次显示帧率信息
                                auto current_time = std::chrono::steady_clock::now();
                                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
                                if (elapsed >= 3 && frame_count > 0) {
                                    double display_fps = frame_count / (double)elapsed;
                                    std::cout << "[DISPLAY_INFO] 显示帧率: " << display_fps << " FPS" << std::endl;
                                    frame_count = 0;
                                    start_time = current_time;
                                }
                            }
                        } catch (...) {
                            // 捕获OpenCV相关异常
                            std::cout << "[WARN] OpenCV操作异常，跳过此帧" << std::endl;
                        }
                    }
                    
                    int key = cv::waitKey(10);
                    
                    // 检查窗口是否被关闭
                    if (cv::getWindowProperty(window_name, cv::WND_PROP_VISIBLE) < 1) {
                        std::cout << "[INFO] 检测到窗口关闭事件" << std::endl;
                        is_stop_ = true;
                        delegate->SetStreamingActive(false);
                        break;
                    }
                    
                    // 按q键或ESC键退出显示
                    if (key == 'q' || key == 27) {
                        is_stop_ = true;
                        delegate->SetStreamingActive(false);
                        std::cout << "[INFO] 退出视频显示，返回主菜单。" << std::endl;
                        break;
                    }
                }
                
                // 安全清理资源
                try {
                    cv::destroyWindow(window_name);
                    cv::waitKey(1);
                } catch (...) {
                    // 忽略窗口关闭时的异常
                }
            } else {
                std::cerr << "[ERROR] 启动推流失败。" << std::endl;
            }
        }

        if (option == 2) {
            std::cout << "[INFO] 正在停止推流..." << std::endl;
            is_stop_ = true;
            delegate->SetStreamingActive(false);
            
            // 安全关闭窗口
            try {
                cv::destroyWindow(window_name);
                cv::waitKey(1);
            } catch (...) {
                // 忽略异常
            }
            
            if (cam->StopLiveStreaming()) {
                stitcher->CancelStitch();
                std::cout << "[INFO] 成功停止推流。" << std::endl;
            }
            else {
                std::cerr << "[ERROR] 停止推流失败。" << std::endl;
            }
        }
    }
    std::cout << "[INFO] 程序即将退出，正在清理资源..." << std::endl;
    
    // 安全清理所有资源
    is_stop_ = true;
    new_frame_ready_.store(false);
    
    try {
        cv::destroyAllWindows();
        cv::waitKey(1);
    } catch (...) {
        // 忽略OpenCV清理异常
    }
    
    try {
        if (cam) {
            cam->Close();
        }
    } catch (...) {
        std::cout << "[WARN] 摄像头关闭时出现异常" << std::endl;
    }
    
    // 断开服务器连接
    if (streamer) {
        streamer->DisconnectVideo();
        streamer->DisconnectAudio();
        std::cout << "[INFO] 已断开服务器连接。" << std::endl;
    }
    
    std::cout << "[INFO] 程序已正常退出。" << std::endl;
    return 0;
}