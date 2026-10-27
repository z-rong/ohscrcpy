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
#include <utility>

#include "hilog/log.h"
#include "capture.h"
#include "server.h"
#include "input.h"
#include "display_manager_lite.h"
#include "virtual_display.h"

#undef LOG_TAG
#define LOG_TAG "ohscrcpy"

namespace {
/** --debug 时 H.264 码流的落盘路径 */
constexpr const char *kDebugDumpPath = "/data/capture.h264";

struct OhscrcpyConfig {
    int32_t     width         = 1920;
    int32_t     height        = 1080;
    int32_t     fps           = 30;
    // 码率（bps）：屏幕内容运动时（滚动/动画）码率会真实用满，12M 对
    // 1200x1920@30 偏紧，默认 20M。注意 RK 类编码器对【静态】画面按内部固定
    // QP 出图、无视码率（实测 12M/20M/40M 关键帧逐字节一致），调码率只影响
    // 运动场景的清晰度。
    int32_t     bitrate       = 20000000;
    // 关键帧间隔（毫秒）：0 会让 GOP=0（不周期刷新 I 帧）导致误差累积、越看越模糊
    int32_t     iFrameIntervalMs = 1000;
    // 码率控制模式：-1=不设置 0=CBR 1=VBR 2=CQ 3=SQR 11=CRF
    int32_t     bitrateMode   = -1;
    // 质量/QP（CQ 的 quality / CRF 的 target_qp；CRF 越小越清晰，建议 18~26）
    int32_t     quality       = -1;
    // 虚拟屏密度 DPI（VPR = dpi/160）：清晰度关键项。
    // 240 => VPR 1.5（UI 按 1280x720 排版后放大，偏糊）；160 => VPR 1.0（1:1 最清晰）
    uint32_t    densityDpi    = 240;
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

/** 把 --rate-mode 的字符串转成 OH VideoEncodeBitrateMode；非法返回 -2 */
int32_t ParseRateMode(const char *s)
{
    if (s == nullptr) { return -2; }
    if (strcmp(s, "cbr") == 0) { return 0; }
    if (strcmp(s, "vbr") == 0) { return 1; }
    if (strcmp(s, "cq") == 0)  { return 2; }
    if (strcmp(s, "sqr") == 0) { return 3; }
    if (strcmp(s, "crf") == 0) { return 11; }
    return -2;
}

void PrintUsage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "\n"
        "选项:\n"
        "  -w <宽>         视频宽度（默认 1920）\n"
        "  -h <高>         视频高度（默认 1080）\n"
        "  -b <码率>       视频码率：>=100000 按 bps，较小值按 Mbps（默认 20Mbps）\n"
        "  -f <帧率>       视频帧率（默认 30）\n"
        "  -p <视频端口>   TCP 视频服务端口（默认 8554）\n"
        "  -c <控制端口>   输入控制端口（默认 8555）\n"
        "  -i <毫秒>       关键帧间隔 ms（默认 1000；调小更清晰但更占带宽，最小建议 500）\n"
        "  --rate-mode <模式>\n"
        "                  码率控制模式 cbr|vbr|cq|sqr|crf（默认不设置）\n"
        "                  本机硬件(OMX.rk.video_encoder.avc)实测只支持 cbr / vbr；\n"
        "                  cq / crf 会被硬件拒绝并导致采集直接失败，不要用\n"
        "  --qp <n>        质量/QP：cq 模式作 quality、crf 模式作 target_qp\n"
        "                  （当前硬件不支持 cq/crf，本项无效）\n"
        "  --density <dpi> 虚拟屏密度（VPR = dpi/160），默认 240(=VPR1.5)\n"
        "                  画面发虚时改小：160 即 VPR 1.0，UI 按 1:1 渲染最清晰\n"
        "  --debug         将编码后的 H.264 码流保存到 %s\n"
        "  --help          打印此帮助\n"
        "\n"
        "示例:\n"
        "  %s -f 60 -b 20 --rate-mode crf --qp 20\n",
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
    cfg.bitrate    = 20000000;
    cfg.iFrameIntervalMs = 1000;
    cfg.videoPort  = 8554;
    cfg.ctrlPort   = 8555;

    // —— 解析命令行参数 ——
    bool userSetResolution = false; // 用户是否手动指定了 -w/-h
    static const struct option kLongOpts[] = {
        {"help",       no_argument, nullptr, 1000},
        {"debug",      no_argument, nullptr, 1001},
        {"rate-mode",  required_argument, nullptr, 1002},
        {"qp",         required_argument, nullptr, 1003},
        {"density",    required_argument, nullptr, 1004},
        {nullptr,      0,          nullptr, 0},
    };

    int opt;
    int longIdx = 0;
    while ((opt = getopt_long(argc, argv, "w:h:f:b:p:c:i:", kLongOpts, &longIdx)) != -1) {
        switch (opt) {
            case 'w': cfg.width    = atoi(optarg); userSetResolution = true; break;
            case 'h': cfg.height   = atoi(optarg); userSetResolution = true; break;
            case 'f': cfg.fps      = atoi(optarg); break;
            case 'b': {
                // 兼容两种写法：>= 100000 视为 bps（如 12000000），较小的数视为 Mbps（如 12）
                int32_t v = atoi(optarg);
                if (v <= 0) {
                    fprintf(stderr, "警告: 码率必须 > 0，忽略 -b %s\n", optarg);
                    break;
                }
                cfg.bitrate = (v >= 100000) ? v : v * 1000000;
                break;
            }
            case 'i': {
                int32_t v = atoi(optarg);
                if (v <= 0) {
                    // 0 会让 GOP=0（不做周期 I 帧刷新），画面会越看越模糊
                    fprintf(stderr, "警告: 关键帧间隔必须 > 0，已改用 1000ms\n");
                    v = 1000;
                }
                cfg.iFrameIntervalMs = v;
                break;
            }
            case 'p': cfg.videoPort = atoi(optarg); break;
            case 'c': cfg.ctrlPort = atoi(optarg); break;
            case 1000:
                PrintUsage(argv[0]);
                return 0;
            case 1001: cfg.debug = true; break;
            case 1002: {
                int32_t m = ParseRateMode(optarg);
                if (m == -2) {
                    fprintf(stderr, "错误: --rate-mode 只支持 cbr|vbr|cq|sqr|crf，收到 \"%s\"\n", optarg);
                    return 1;
                }
                cfg.bitrateMode = m;
                break;
            }
            case 1003: {
                int32_t v = atoi(optarg);
                if (v < 0) {
                    fprintf(stderr, "警告: --qp 必须 >= 0，忽略\n");
                    break;
                }
                cfg.quality = v;
                break;
            }
            case 1004: {
                int32_t v = atoi(optarg);
                if (v <= 0) {
                    fprintf(stderr, "警告: --density 必须 > 0，忽略\n");
                    break;
                }
                cfg.densityDpi = static_cast<uint32_t>(v);
                break;
            }
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    // —— 判断设备是否有【真实】屏幕 ——
    // 不能只看「有没有默认屏」：本工具自己创建的虚拟屏也是默认屏（尤其是上次进程被强杀
    // 时留下的那块），那样会误判为「有真实屏」而走 AVScreenCapture 并再次失败。
    const bool hasDisplay = ohscrcpy::HasRealDisplay();
    if (!hasDisplay) {
        HILOG_WARN(LOG_CORE, "未检测到真实屏幕（设备无物理显示器），将使用虚拟屏直连编码器模式");
    }

    // —— 有真实屏幕时查询分辨率与旋转角度 ——
    // 无屏时虚拟屏尺寸就是我们请求的 cfg.width x cfg.height，无需查询。
    // 注意 GetRotation() 返回的是档位枚举（ROTATION_0..ROTATION_270 = 0..3），
    // 乘 90 才是角度。
    int32_t displayRotation = 0; // 屏幕旋转角（0/90/180/270），随协议头下发给客户端
    if (hasDisplay) {
        auto display = OHOS::Rosen::DisplayManagerLite::GetInstance().GetDefaultDisplay();
        if (display != nullptr) {
            int32_t w = display->GetWidth();
            int32_t h = display->GetHeight();
            if (w > 0 && h > 0 && !userSetResolution) {
                cfg.width  = w;
                cfg.height = h;
                HILOG_INFO(LOG_CORE, "真实屏幕分辨率: %{public}dx%{public}d", w, h);
            } else if (w <= 0 || h <= 0) {
                HILOG_WARN(LOG_CORE, "获取屏幕分辨率失败，使用默认 %{public}dx%{public}d",
                           cfg.width, cfg.height);
            }
            displayRotation = static_cast<int32_t>(display->GetRotation()) * 90;
            HILOG_INFO(LOG_CORE, "屏幕旋转角度: %{public}d", displayRotation);
        } else {
            HILOG_WARN(LOG_CORE, "获取屏幕分辨率失败，使用默认 %{public}dx%{public}d",
                       cfg.width, cfg.height);
        }
    }

    // —— 竖屏物理面板当横屏用的设备（cvt 等，rotation=90/270）——
    // 录制镜像按「逻辑横屏」请求画布时，RS 会把正立内容再旋转屏幕角度并等比
    // 缩放居中，两侧补黑边（画面利用率只有 ~39%）；只有按「物理面板方向」请求
    // 画布才能满屏出图（内容带屏幕旋转角）。所以这里把采集分辨率交换成物理面板
    // 方向，旋转角度随协议头下发，由客户端转正显示；转正后的尺寸恰好等于逻辑
    // 分辨率，客户端反控坐标无需额外变换。
    if (displayRotation == 90 || displayRotation == 270) {
        if (!userSetResolution) {
            std::swap(cfg.width, cfg.height);
            HILOG_INFO(LOG_CORE,
                       "检测到旋转屏（rotation=%{public}d），按物理面板方向采集: %{public}dx%{public}d",
                       displayRotation, cfg.width, cfg.height);
        }
    } else {
        displayRotation = 0;
    }

    HILOG_INFO(LOG_CORE,
        "ohscrcpy 启动: %{public}dx%{public}d @%{public}dfps, 码率=%{public}dbps, "
        "关键帧间隔=%{public}dms, 码率模式=%{public}d, 质量/QP=%{public}d, "
        "视频端口=%{public}d, 控制端口=%{public}d",
        cfg.width, cfg.height, cfg.fps, cfg.bitrate, cfg.iFrameIntervalMs,
        cfg.bitrateMode, cfg.quality, cfg.videoPort, cfg.ctrlPort);

    // —— 注册信号（恢复默认行为，不拦截）——
    // 不注册自定义 SignalHandler，SIGINT/SIGTERM 直接终止进程，
    // 避免信号回调中持锁导致死锁

    // —— 启动 TCP 视频服务器（固定使用 Unix Domain Socket abstract 命名空间）——
    auto video = std::make_unique<Server>(cfg.videoPort, cfg.width, cfg.height,
                                          true, cfg.videoSockName);
    // 旋转角度随每帧帧头下发，客户端据此转正画面（cvt 等旋转屏设备非 0）
    video->SetRotation(displayRotation);
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
    capCfg.bitrate = cfg.bitrate;
    capCfg.iFrameIntervalMs = cfg.iFrameIntervalMs;
    capCfg.bitrateMode = cfg.bitrateMode;
    capCfg.quality = cfg.quality;
    // 屏幕旋转角：非 0 时 capture.cpp 会关闭 canvasFollowRotation（按请求方向满屏
    // 出图），并把它标记进编码器；客户端转正显示用的角度来自协议头
    capCfg.rotation = displayRotation;
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

    if (hasDisplay) {
        // 有真实屏幕：走原有 AVScreenCapture 流程
        if (!capture->Start()) {
            HILOG_ERROR(LOG_CORE, "屏幕采集启动失败");
            return 1;
        }
        HILOG_INFO(LOG_CORE, "屏幕采集已启动");
    } else {
        // 无物理显示器：建一块虚拟屏，并把「编码器输入 Surface」交给它，
        // 由 RS 把该屏的合成结果直接渲染进编码器 —— 完全绕开 AVScreenCapture。
        auto encoderSurface = capture->PrepareEncoderForExternalInput();
        if (encoderSurface == nullptr) {
            HILOG_ERROR(LOG_CORE, "编码器输入 Surface 创建失败，无法建立虚拟屏");
            return 1;
        }
        if (!ohscrcpy::CreateVirtualDisplayAsDefault(cfg.width, cfg.height, encoderSurface,
                                                     cfg.densityDpi)) {
            HILOG_ERROR(LOG_CORE, "虚拟屏创建失败");
            return 1;
        }
        capture->StartExternalInput();
        HILOG_INFO(LOG_CORE, "屏幕采集已启动（虚拟屏直连编码器 Surface）");
    }

    // 明确告诉输入服务该往哪块屏注入：无屏设备上是刚建好的默认虚拟屏，
    // 有屏设备上就是真实默认屏。不指定时 MMI 只按「默认显示组第一个屏」推断，
    // 多屏（物理屏 + 虚拟屏、或遗留旧虚拟屏）时会注入到别的屏上 —— 表现为
    // 「画面能看、点按没反应」。
    int32_t targetDisplayId = ohscrcpy::QueryDefaultDisplayId();
    inputSrv->SetTargetDisplayId(targetDisplayId);
    HILOG_INFO(LOG_CORE, "输入注入目标屏幕 displayId=%{public}d", targetDisplayId);

    // 新客户端连接时请求 IDR 帧，确保客户端能立即收到 SPS/PPS
    video->SetNewClientCallback([&capture]() {
        if (capture) { capture->RequestIDRFrame(); }
    });

    HILOG_INFO(LOG_CORE, "ohscrcpy 运行中，按 Ctrl+C 退出");
    pause();

    if (h264Fd >= 0) { close(h264Fd); }
    // 回收本进程创建的虚拟屏。注意：虚拟屏已被设为默认屏，DMS 的
    // DestroyVirtualScreen 对默认屏一律拒绝（dmserver/abstract_screen_controller.cpp
    // 检查 rsScreenId == defaultRsScreenId_，含进程被杀时的 OnRemoteDied 兜底路径），
    // 所以这里通常只是打一条失败日志；遗留屏由下次启动 FindOurVirtualScreen 复用。
    ohscrcpy::DestroyVirtualDisplay();
    HILOG_INFO(LOG_CORE, "ohscrcpy 已退出");
    return 0;
}
