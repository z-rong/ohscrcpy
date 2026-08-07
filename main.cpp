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
 *   1. 屏幕采集  (screen_capture.cpp)
 *      - 使用 OH AVScreenCapture C API 采集全屏幕
 *      - 配置 dataType = OH_ENCODED_STREAM + videoCodec = OH_H264，直接获取 H264 裸流
 *      - 通过回调 OnBufferAvailable 将视频帧推送给 RTSP 服务器
 *
 *   2. RTSP 服务器  (server.cpp)
 *      - 内置轻量级 RTSP/RTP 服务器（基于 TCP/UDP，不依赖第三方库）
 *      - SDP 描述 H264 视频轨道
 *      - 支持多客户端连接，RTSP 端口默认 8554
 *      - 将屏幕采集的 H264 NALU 打包为 RTP 包（RFC 3984 FU-A 分片）进行分发
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
 *   -w <width>      视频宽度（默认 1280）
 *   -h <height>     视频高度（默认 720）
 *   -b <bitrate>    视频码率，单位 bps（默认 4000000）
 *   -f <fps>        帧率（默认 30）
 *   -p <rtsp_port>  RTSP 服务端口（默认 8554）
 *   -c <ctrl_port>  控制服务端口（默认 8555）
 *   --no-capture    不启动屏幕采集（仅测试 RTSP/控制服务）
 *   --no-rtsp       不启动 RTSP 服务器
 *   --no-input      不启动输入控制服务
 *   --help          打印帮助信息
 *
 * 退出：按 Ctrl+C 或发送 SIGINT 信号，程序优雅退出并释放所有资源。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <getopt.h>
#include <unistd.h>
#include <memory>

#include "hilog/log.h"
#include "screen_capture.h"
#include "server.h"
#include "input.h"
#include "display_manager_lite.h"

#undef LOG_TAG
#define LOG_TAG "ohscrcpy"

namespace {
struct OhscrcpyConfig {
    int32_t     width         = 1280;
    int32_t     height        = 720;
    int32_t     fps           = 30;
    int32_t     rtspPort      = 8554;
    int32_t     ctrlPort      = 8555;
    bool        enableCapture = true;
    bool        enableRtsp    = true;
    bool        enableInput   = true;
    bool        useAbstract   = false;  // 使用 Unix Domain Socket abstract
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
        "  -w <宽>         视频宽度（默认 1280）\n"
        "  -h <高>         视频高度（默认 720）\n"
        "  -b <码率>       视频码率 bps（默认 4000000）\n"
        "  -f <帧率>       视频帧率（默认 30）\n"
        "  -p <rtsp端口>   RTSP 服务端口（默认 8554）\n"
        "  -c <控制端口>   输入控制端口（默认 8555）\n"
        "  --abstract      使用 Unix Domain Socket abstract 命名空间\n"
        "                  配合 hdc fport localabstract 使用，延迟更低\n"
        "  --no-capture    不启动屏幕采集\n"
        "  --no-rtsp       不启动 RTSP 服务\n"
        "  --no-input      不启动输入控制服务\n"
        "  --help          打印此帮助\n"
        "\n"
        "示例:\n"
        "  %s -w 1920 -h 1080 -b 8000000 -f 60\n",
        prog, prog);
}
} // namespace

int main(int argc, char *argv[])
{
    // —— 默认参数 ——
    OhscrcpyConfig cfg{};
    cfg.width      = 1280;
    cfg.height     = 720;
    cfg.fps        = 30;
    cfg.rtspPort   = 8554;
    cfg.ctrlPort   = 8555;
    cfg.enableCapture = true;
    cfg.enableRtsp    = true;
    cfg.enableInput   = true;

    // —— 解析命令行参数 ——
    static const struct option kLongOpts[] = {
        {"no-capture", no_argument, nullptr, 1000},
        {"no-rtsp",    no_argument, nullptr, 1001},
        {"no-input",   no_argument, nullptr, 1002},
        {"help",       no_argument, nullptr, 1003},
        {"abstract",   no_argument, nullptr, 1004},
        {nullptr,      0,          nullptr, 0},
    };

    int opt;
    int longIdx = 0;
    while ((opt = getopt_long(argc, argv, "w:h:f:p:c:", kLongOpts, &longIdx)) != -1) {
        switch (opt) {
            case 'w': cfg.width    = atoi(optarg); break;
            case 'h': cfg.height   = atoi(optarg); break;
            case 'f': cfg.fps      = atoi(optarg); break;
            case 'p': cfg.rtspPort = atoi(optarg); break;
            case 'c': cfg.ctrlPort = atoi(optarg); break;
            case 1000: cfg.enableCapture = false; break;
            case 1001: cfg.enableRtsp    = false; break;
            case 1002: cfg.enableInput   = false; break;
            case 1003:
                PrintUsage(argv[0]);
                return 0;
            case 1004: cfg.useAbstract   = true;  break;
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    // —— 查询真实屏幕分辨率（用户未手动指定时自动获取）——
    if (cfg.width == 1280 && cfg.height == 720) {
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
        "RTSP端口=%{public}d, 控制端口=%{public}d",
        cfg.width, cfg.height, cfg.fps, cfg.rtspPort, cfg.ctrlPort);

    // —— 注册信号（恢复默认行为，不拦截）——
    // 不注册自定义 SignalHandler，SIGINT/SIGTERM 直接终止进程，
    // 避免信号回调中持锁导致死锁

    // —— 启动 RTSP 服务器 ——
    std::unique_ptr<Server> rtsp;
    if (cfg.enableRtsp) {
        rtsp = std::make_unique<Server>(cfg.rtspPort, cfg.width, cfg.height,
                                            cfg.useAbstract, cfg.videoSockName);
        if (!rtsp->Start()) {
            HILOG_ERROR(LOG_CORE, "RTSP 服务器启动失败");
            return 1;
        }
        if (cfg.useAbstract) {
            HILOG_INFO(LOG_CORE, "RTSP 服务器已启动（abstract: %{public}s）", cfg.videoSockName.c_str());
        } else {
            HILOG_INFO(LOG_CORE, "RTSP 服务器已启动，端口 %{public}d", cfg.rtspPort);
        }
    }

    // —— 启动输入控制服务 ——
    std::unique_ptr<InputServer> inputSrv;
    if (cfg.enableInput) {
        inputSrv = std::make_unique<InputServer>(cfg.ctrlPort, cfg.width, cfg.height,
                                                 cfg.useAbstract, cfg.ctrlSockName);
        if (!inputSrv->Start()) {
            HILOG_ERROR(LOG_CORE, "输入控制服务启动失败");
            return 1;
        }
        if (cfg.useAbstract) {
            HILOG_INFO(LOG_CORE, "输入控制服务已启动（abstract: %{public}s）", cfg.ctrlSockName.c_str());
        } else {
            HILOG_INFO(LOG_CORE, "输入控制服务已启动，端口 %{public}d", cfg.ctrlPort);
        }
    }

    // —— 启动屏幕采集 ——
    // 采集回调直接将 H264 帧推送到 Server
    std::unique_ptr<ScreenCapture> capture;
    if (cfg.enableCapture) {
        ScreenCaptureConfig scCfg{};
        scCfg.width   = cfg.width;
        scCfg.height  = cfg.height;
        scCfg.fps     = cfg.fps;
        // H.264 NALU 回调 —— 推送到 Server
        scCfg.onNalu = [&](const uint8_t *data, int32_t size,
                            int64_t pts, bool isKey) {
            if (rtsp) {
                rtsp->PushNalu(data, size, pts, isKey);
            }
        };

        capture = std::make_unique<ScreenCapture>(scCfg);
        if (!capture->Start()) {
            HILOG_ERROR(LOG_CORE, "屏幕采集启动失败");
            return 1;
        }
        HILOG_INFO(LOG_CORE, "屏幕采集已启动");

        // 新客户端连接时请求 IDR 帧，确保客户端能立即收到 SPS/PPS
        if (rtsp) {
            rtsp->SetNewClientCallback([&capture]() {
                if (capture) { capture->RequestIDRFrame(); }
            });
        }
    }

    // —— 主循环：永久阻塞，等待 SIGINT/SIGTERM 终止进程 ——
    HILOG_INFO(LOG_CORE, "ohscrcpy 运行中，按 Ctrl+C 退出");
    pause(); // 阻塞直到收到任意信号，系统默认行为终止进程

    // 以下代码在正常信号退出时不会执行（系统直接终止），
    // 保留仅用于未来改为优雅退出时参考
    HILOG_INFO(LOG_CORE, "ohscrcpy 已退出");
    return 0;
}
