/*
 * Copyright (c) 2024-2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file main.cpp
 * @brief ohscrcpy —— OpenHarmony 投屏工具入口
 *
 * 功能概述：
 *   1. 屏幕采集  (capture.cpp)
 *      - 使用 OH AVScreenCapture C API 采集全屏幕
 *      - 配置 dataType = OH_ENCODED_STREAM + videoCodec = OH_H264，直接获取 H264 裸流
 *      - 通过回调 OnBufferAvailable 将视频帧推送给 TCP 视频服务器
 *
 *   2. TCP 视频服务器  (server.cpp)
 *      - 内置轻量级 TCP 服务器（不依赖第三方库），支持 TCP 端口或 Unix Domain Socket
 *      - 帧协议（小端序）：
 *          magic(4) + flags(1) + pts(8) + size(4) + width(4) + height(4) + H.264 NALU
 *      - 支持多客户端连接，视频端口默认 8554
 *      - 新客户端接入时先补发 SPS/PPS 与最近的 IDR 帧，保证立即出画
 *
 *   3. 控制服务  (input.cpp)
 *      - 监听 TCP 控制端口（默认 8555）
 *      - 接收客户端发来的输入控制协议消息（鼠标移动、点击、触摸、按键）
 *      - 调用 OH_Input_InjectMouseEvent / OH_Input_InjectTouchEvent 注入系统输入事件
 *
 * 使用方法：
 *   ohscrcpy [选项]
 *
 * 选项：
 *   -w <width>      视频宽度（默认 1920）
 *   -h <height>     视频高度（默认 1080）
 *   -b <bitrate>    视频码率，单位 bps（默认 4000000）
 *   -f <fps>        帧率（默认 30）
 *   -p <video_port> TCP 视频服务端口（默认 8554）
 *   -c <ctrl_port>  控制服务端口（默认 8555）
 *   --debug         将编码后的 H.264 码流保存到 /data/capture.h264
 *   --help          打印帮助信息
 *
 * 退出：按 Ctrl+C 或发送 SIGINT 信号，程序优雅退出并释放所有资源。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <memory>

#include "hilog/log.h"
#include "capture.h"
#include "server.h"
#include "input.h"
#include "display_manager_lite.h"

#undef LOG_TAG
#define LOG_TAG "ohscrcpy"

namespace {
/** --debug 时 H.264 码流的落盘路径 */
constexpr const char *kDebugDumpPath = "/data/capture.h264";

struct OhscrcpyConfig {
    int32_t     width         = 1920;
    int32_t     height        = 1080;
    int32_t     fps           = 30;
    int32_t     videoPort     = 8554;
    int32_t     ctrlPort      = 8555;
    bool        debug         = false;  // 将编码后的 H.264 码流保存到 kDebugDumpPath
    std::string videoSockName = "ohscrcpy_video";
    std::string ctrlSockName  = "ohscrcpy_ctrl";
};
} // namespace
#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD002B00

namespace {

void PrintUsage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "\n"
        "选项:\n"
        "  -w <宽>         视频宽度（默认 1920）\n"
        "  -h <高>         视频高度（默认 1080）\n"
        "  -b <码率>       视频码率 bps（默认 4000000）\n"
        "  -f <帧率>       视频帧率（默认 30）\n"
        "  -p <视频端口>   TCP 视频服务端口（默认 8554）\n"
        "  -c <控制端口>   输入控制端口（默认 8555）\n"
        "  --debug         将编码后的 H.264 码流保存到 %s\n"
        "  --help          打印此帮助\n"
        "\n"
        "示例:\n"
        "  %s -f 60\n",
        kDebugDumpPath, prog, prog);
}
} // namespace

int main(int argc, char *argv[])
{
    // —— 默认参数 ——
    OhscrcpyConfig cfg{};
    cfg.width      = 1920;
    cfg.height     = 1080;
    cfg.fps        = 30;
    cfg.videoPort  = 8554;
    cfg.ctrlPort   = 8555;

    // —— 解析命令行参数 ——
    static const struct option kLongOpts[] = {
        {"help",       no_argument, nullptr, 1000},
        {"debug",      no_argument, nullptr, 1001},
        {nullptr,      0,          nullptr, 0},
    };

    int opt;
    int longIdx = 0;
    while ((opt = getopt_long(argc, argv, "w:h:f:p:c:", kLongOpts, &longIdx)) != -1) {
        switch (opt) {
            case 'w': cfg.width    = atoi(optarg); break;
            case 'h': cfg.height   = atoi(optarg); break;
            case 'f': cfg.fps      = atoi(optarg); break;
            case 'p': cfg.videoPort = atoi(optarg); break;
            case 'c': cfg.ctrlPort = atoi(optarg); break;
            case 1000:
                PrintUsage(argv[0]);
                return 0;
            case 1001: cfg.debug = true; break;
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    // —— 查询真实屏幕分辨率（用户未手动指定时自动获取）——
    if (cfg.width == 1920 && cfg.height == 1080) {
        auto display = OHOS::Rosen::DisplayManagerLite::GetInstance().GetDefaultDisplay();
        if (display != nullptr) {
            int32_t w = display->GetWidth();
            int32_t h = display->GetHeight();
            if (w > 0 && h > 0) {
                cfg.width  = w;
                cfg.height = h;
                HILOG_INFO(LOG_CORE, "真实屏幕分辨率: %{public}dx%{public}d", w, h);
            }
        } else {
            HILOG_WARN(LOG_CORE, "获取屏幕分辨率失败，使用默认 %{public}dx%{public}d",
                       cfg.width, cfg.height);
        }
    }

    HILOG_INFO(LOG_CORE,
        "ohscrcpy 启动: %{public}dx%{public}d @%{public}dfps, "
        "视频端口=%{public}d, 控制端口=%{public}d",
        cfg.width, cfg.height, cfg.fps, cfg.videoPort, cfg.ctrlPort);

    // —— 注册信号（恢复默认行为，不拦截）——
    // 不注册自定义 SignalHandler，SIGINT/SIGTERM 直接终止进程，
    // 避免信号回调中持锁导致死锁

    // —— 启动 TCP 视频服务器（固定使用 Unix Domain Socket abstract 命名空间）——
    auto video = std::make_unique<Server>(cfg.videoPort, cfg.width, cfg.height,
                                          true, cfg.videoSockName);
    if (!video->Start()) {
        HILOG_ERROR(LOG_CORE, "TCP 视频服务器启动失败");
        return 1;
    }
    HILOG_INFO(LOG_CORE, "TCP 视频服务器已启动（abstract: %{public}s）",
               cfg.videoSockName.c_str());

    // —— 启动输入控制服务（固定使用 Unix Domain Socket abstract 命名空间）——
    auto inputSrv = std::make_unique<InputServer>(cfg.ctrlPort, cfg.width, cfg.height,
                                                  true, cfg.ctrlSockName);
    if (!inputSrv->Start()) {
        HILOG_ERROR(LOG_CORE, "输入控制服务启动失败");
        return 1;
    }
    HILOG_INFO(LOG_CORE, "输入控制服务已启动（abstract: %{public}s）",
               cfg.ctrlSockName.c_str());

    // —— 调试模式：编码后的 H.264 码流落盘 ——
    int h264Fd = -1;
    if (cfg.debug) {
        h264Fd = open(kDebugDumpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (h264Fd < 0) {
            HILOG_ERROR(LOG_CORE, "打开 %{public}s 失败: %{public}s",
                        kDebugDumpPath, strerror(errno));
        } else {
            HILOG_INFO(LOG_CORE, "调试模式已开启，码流保存到 %{public}s", kDebugDumpPath);
        }
    }

    // —— 启动屏幕采集 ——
    // 采集回调直接将 H264 帧推送到 Server，debug 模式下同时写入文件
    CaptureConfig capCfg{};
    capCfg.width  = cfg.width;
    capCfg.height = cfg.height;
    capCfg.fps    = cfg.fps;
    // H.264 NALU 回调 —— 推送到 TCP 视频服务器
    capCfg.onNalu = [&video, h264Fd](const uint8_t *data, int32_t size,
                                     int64_t pts, bool isKey) {
        if (h264Fd >= 0) {
            // 直接 write 到 fd，不经过 stdio 缓冲：即使进程被信号直接杀死，
            // 已经写出的数据也不会丢失，落盘文件随时可播放。
            ssize_t written = ::write(h264Fd, data, static_cast<size_t>(size));
            if (written != static_cast<ssize_t>(size)) {
                HILOG_ERROR(LOG_CORE, "写入 %{public}s 失败", kDebugDumpPath);
            }
        }
        video->PushNalu(data, size, pts, isKey);
    };

    auto capture = std::make_unique<Capture>(capCfg);
    if (!capture->Start()) {
        HILOG_ERROR(LOG_CORE, "屏幕采集启动失败");
        return 1;
    }
    HILOG_INFO(LOG_CORE, "屏幕采集已启动");

    // 新客户端连接时请求 IDR 帧，确保客户端能立即收到 SPS/PPS
    video->SetNewClientCallback([&capture]() {
        if (capture) { capture->RequestIDRFrame(); }
    });

    HILOG_INFO(LOG_CORE, "ohscrcpy 运行中，按 Ctrl+C 退出");
    pause();

    if (h264Fd >= 0) { close(h264Fd); }
    HILOG_INFO(LOG_CORE, "ohscrcpy 已退出");
    return 0;
}
