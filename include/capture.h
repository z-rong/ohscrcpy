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

#ifndef OHSCRCPY_CAPTURE_H
#define OHSCRCPY_CAPTURE_H

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>

#include "surface.h"
#include "screen_capture.h"
#include "avcodec_common.h"
#include "avcodec_video_encoder.h"
#include "meta/format.h"
#include "meta/media_types.h"

namespace ohscrcpy {

/**
 * H.264 NALU 回调
 *   data  — NALU 数据
 *   size  — 字节数
 *   pts   — 时间戳（微秒）
 *   isKey — 是否关键帧（IDR）
 */
using NaluCallback = std::function<void(const uint8_t *data, int32_t size,
                                        int64_t pts, bool isKey)>;

/** 屏幕采集 + 编码配置 */
struct CaptureConfig {
    int32_t width    = 1920;
    int32_t height   = 1080;
    int32_t fps      = 30;
    // 码率（bps）。屏幕内容运动时码率会真实用满，默认 20Mbps。
    // 注意 RK 类硬编码器对静态画面按内部固定 QP 出图、无视该值（实测
    // 12M/20M/40M 静态关键帧逐字节一致），调码率只影响运动场景清晰度。
    int32_t bitrate  = 20000000;
    // 关键帧间隔（毫秒）。codec 侧按 gopSize = fps * iFrameIntervalMs / 1000 换算。
    // 注意：传 0 会让 GOP 变成 0（即不做周期性 I 帧刷新），P 帧量化误差持续累积，
    // 画面会越看越模糊；传负数更极端（gopSize=INT32_MAX）。OH 默认值就是 1000。
    int32_t iFrameIntervalMs = 1000;
    // 码率控制模式（对应 OH VideoEncodeBitrateMode）：-1=不设置（用编码器默认）
    //   0=CBR  1=VBR  2=CQ（恒定质量）  3=SQR  11=CRF（恒定码率因子）
    // 为什么重要：屏幕内容在「静态画面」下并不会有很高的瞬时码率需求，
    // 单靠调大码率上限并不会变清晰；只有 CQ/CRF 这类「按质量分配码率」的模式
    // 才会真正降低量化步长。硬件编码器收到该 tag 后会走
    // SetConstantQualityMode() / SetCRFMode()（见 hcodec 的 ConfigureOutputBitrate）。
    int32_t bitrateMode = -1;
    // 质量/QP：CQ 模式下作为 quality，CRF 模式下作为 target_qp。
    // 数值语义随模式不同（CRF 是 QP，越小越清晰，建议 18~26）；-1 表示不设置。
    int32_t quality = -1;
    int32_t rotation = 0;        // 视频旋转角度（0/90/180/270），用于竖屏当横屏等场景
    NaluCallback onNalu;
};

class Capture {
public:
    explicit Capture(const CaptureConfig &cfg);
    ~Capture();

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    /** 请求编码器立即输出一个 IDR 帧（含 SPS/PPS），供新客户端连接时调用 */
    void RequestIDRFrame();

    /**
     * 「虚拟屏直连」模式：只初始化编码器（Surface 输入），不启动 AVScreenCapture，
     * 返回编码器输入 Surface。
     *
     * 调用方须把它交给虚拟屏（VirtualScreenOption::surface_），RS 会把虚拟屏的
     * 合成结果直接渲染进这个 Surface，编码器随即产出 H.264。
     *
     * 为什么需要这个模式：AVScreenCapture 内部会对默认屏做 MakeMirror，而 MakeMirror
     * 拒绝虚拟屏作为镜像源
     * （abstract_screen_controller.cpp: "screen is nullptr, or screenType is not real."），
     * 所以无物理显示器时无法用 AVScreenCapture 采集虚拟屏。
     *
     * @return 编码器输入 Surface；失败返回 nullptr
     */
    OHOS::sptr<OHOS::Surface> PrepareEncoderForExternalInput();

    /**
     * 外部画面源（虚拟屏）已接到编码器输入 Surface：标记为运行中并请求一次 IDR
     *（含 SPS/PPS）。必须在视频服务器的接收端就绪之后再调用，否则首个 SPS/PPS 会被丢弃。
     */
    void StartExternalInput();

private:
    // ── 采集回调（继承框架 ScreenCaptureCallBack）───────────────────────────
    class CaptureCallback : public OHOS::Media::ScreenCaptureCallBack {
    public:
        explicit CaptureCallback(Capture *owner) : owner_(owner) {}
        void OnError(OHOS::Media::ScreenCaptureErrorType errorType,
                     int32_t errorCode) override;
        void OnAudioBufferAvailable(bool isReady,
                                    OHOS::Media::AudioCaptureSourceType type) override;
        void OnVideoBufferAvailable(bool isReady) override;
        void OnStateChange(OHOS::Media::AVScreenCaptureStateCode stateCode) override;
    private:
        Capture *owner_;
    };

    // ── MediaCodecCallback 内部类（C++ 编码器回调）───────────────────────────
    class EncCallback : public OHOS::MediaAVCodec::MediaCodecCallback {
    public:
        explicit EncCallback(Capture *owner) : owner_(owner) {}
        void OnError(OHOS::MediaAVCodec::AVCodecErrorType type,
                     int32_t errorCode) override;
        void OnOutputFormatChanged(const OHOS::Media::Format &format) override;
        // Surface 输入模式下编码器不再回调 OnInputBufferAvailable
        void OnInputBufferAvailable(uint32_t index,
                                    std::shared_ptr<OHOS::Media::AVBuffer> buffer) override {}
        void OnOutputBufferAvailable(uint32_t index,
                                     std::shared_ptr<OHOS::Media::AVBuffer> buffer) override;
    private:
        Capture *owner_;
    };

    bool InitEncoder();
    void DestroyEncoder();

    CaptureConfig cfg_;
    std::shared_ptr<OHOS::Media::ScreenCapture> capture_;
    std::shared_ptr<OHOS::MediaAVCodec::AVCodecVideoEncoder> encoder_;
    std::atomic<bool> running_{false};

    // 编码器 Surface 输入句柄（Surface 模式直传给采集器）
    OHOS::sptr<OHOS::Surface> encoderSurface_;
};

} // namespace ohscrcpy

using CaptureConfig = ohscrcpy::CaptureConfig;
using Capture       = ohscrcpy::Capture;

#endif // OHSCRCPY_CAPTURE_H
