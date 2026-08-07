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

#ifndef OHSCRCPY_SCREEN_CAPTURE_H
#define OHSCRCPY_SCREEN_CAPTURE_H

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>

#include "surface.h"

// C++ AVScreenCapture inner API（用相对路径避免与本文件同名冲突）
#include "../../interfaces/inner_api/native/screen_capture.h"
// C++ AVCodecVideoEncoder inner API
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
struct ScreenCaptureConfig {
    int32_t width    = 1280;
    int32_t height   = 720;
    int32_t fps      = 30;
    int32_t bitrate  = 4000000; // bps
    int32_t rotation = 0;        // 视频旋转角度（0/90/180/270），用于竖屏当横屏等场景
    NaluCallback onNalu;
};

/**
 * @brief 屏幕采集（C++ inner API）+ H.264 编码（C++ inner API）封装
 *
 * 流程（Surface 模式零拷贝）：
 *   1. InitEncoder → CreateInputSurface() 得到编码器 Surface
 *   2. Start → StartScreenCaptureWithSurface(encoderSurface_)
 *      采集器直接将帧写入编码器 Surface，无需回调中转
 *   3. 编码器从 Surface 直接消费，OnOutputBufferAvailable → onNalu
 *   CPU 全程不接触像素数据，无 memcpy
 */
class ScreenCapture {
public:
    explicit ScreenCapture(const ScreenCaptureConfig &cfg);
    ~ScreenCapture();

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    /** 请求编码器立即输出一个 IDR 帧（含 SPS/PPS），供新客户端连接时调用 */
    void RequestIDRFrame();

private:
    // ── C++ ScreenCaptureCallBack 内部类 ────────────────────────────────────
    class CaptureCallback : public OHOS::Media::ScreenCaptureCallBack {
    public:
        explicit CaptureCallback(ScreenCapture *owner) : owner_(owner) {}
        void OnError(OHOS::Media::ScreenCaptureErrorType errorType,
                     int32_t errorCode) override;
        void OnAudioBufferAvailable(bool isReady,
                                    OHOS::Media::AudioCaptureSourceType type) override;
        void OnVideoBufferAvailable(bool isReady) override;
        void OnStateChange(OHOS::Media::AVScreenCaptureStateCode stateCode) override;
    private:
        ScreenCapture *owner_;
    };

    // ── MediaCodecCallback 内部类（C++ 编码器回调）───────────────────────────
    class EncCallback : public OHOS::MediaAVCodec::MediaCodecCallback {
    public:
        explicit EncCallback(ScreenCapture *owner) : owner_(owner) {}
        void OnError(OHOS::MediaAVCodec::AVCodecErrorType type,
                     int32_t errorCode) override;
        void OnOutputFormatChanged(const OHOS::Media::Format &format) override;
        // Surface 输入模式下编码器不再回调 OnInputBufferAvailable
        void OnInputBufferAvailable(uint32_t index,
                                    std::shared_ptr<OHOS::Media::AVBuffer> buffer) override {}
        void OnOutputBufferAvailable(uint32_t index,
                                     std::shared_ptr<OHOS::Media::AVBuffer> buffer) override;
    private:
        ScreenCapture *owner_;
    };

    bool InitEncoder();
    void DestroyEncoder();

    ScreenCaptureConfig cfg_;
    std::shared_ptr<OHOS::Media::ScreenCapture> capture_;
    std::shared_ptr<OHOS::MediaAVCodec::AVCodecVideoEncoder> encoder_;
    std::atomic<bool> running_{false};

    // 编码器 Surface 输入句柄（Surface 模式直传给采集器）
    OHOS::sptr<OHOS::Surface> encoderSurface_;
};

} // namespace ohscrcpy

using ScreenCaptureConfig  = ohscrcpy::ScreenCaptureConfig;
using ScreenCapture = ohscrcpy::ScreenCapture;

#endif // OHSCRCPY_SCREEN_CAPTURE_H
