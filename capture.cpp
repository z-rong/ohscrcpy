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
 * @file capture.cpp
 * @brief 屏幕采集（C++ inner API）+ H.264 编码（C++ inner API）实现
 *
 * 流程（Surface 模式零拷贝）：
 *   1. InitEncoder → CreateInputSurface() 获取编码器消费端 Surface
 *   2. Start → StartScreenCaptureWithSurface(encoderSurface_)
 *      采集器直接将帧写入编码器 Surface，无需回调中转
 *   3. EncCallback::OnOutputBufferAvailable 收到 H.264 NALU → onNalu 回调
 *
 *   CPU 全程不读写像素数据，彻底消除 memcpy 开销。
 *   相比 ORIGINAL_STREAM + OnVideoBufferAvailable 手动中转，Surface 模式
 *   减少一次 BufferQueue 流转，延迟更低。
 */

#include "capture.h"

#include <cstdio>

#include "hilog/log.h"
// C++ 编码器工厂
#include "avcodec_video_encoder.h"
#include "avcodec_common.h"
#include "avcodec_errors.h"
#include "meta/format.h"
#include "meta/media_types.h"
#include "meta/meta_key.h"
#include "meta/video_types.h"

#undef LOG_TAG
#define LOG_TAG "ohscrcpy_capture"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD002B01

using namespace OHOS::MediaAVCodec;
using namespace OHOS::Media;

namespace ohscrcpy {

Capture::Capture(const CaptureConfig &cfg)
    : cfg_(cfg)
{
}

Capture::~Capture()
{
    Stop();
}

// ── 编码器初始化（Surface 输入模式）────────────────────────────────────────
bool Capture::InitEncoder()
{
    encoder_ = VideoEncoderFactory::CreateByMime(
        std::string(CodecMimeType::VIDEO_AVC));
    if (encoder_ == nullptr) {
        HILOG_ERROR(LOG_CORE, "VideoEncoderFactory::CreateByMime 失败");
        return false;
    }

    auto cb = std::make_shared<EncCallback>(this);
    int32_t ret = encoder_->SetCallback(cb);
    if (ret != AVCS_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "SetCallback 失败: %{public}d", ret);
        encoder_->Release();
        encoder_.reset();
        return false;
    }

    Format fmt;
    fmt.PutIntValue(Tag::VIDEO_WIDTH,            cfg_.width);
    fmt.PutIntValue(Tag::VIDEO_HEIGHT,           cfg_.height);
    // Surface 输入模式像素格式由 Surface 决定，编码器侧设 NV12 或 RGBA 均可
    fmt.PutIntValue(Tag::VIDEO_PIXEL_FORMAT,     static_cast<int32_t>(OHOS::Media::VideoPixelFormat::RGBA));
    fmt.PutDoubleValue(Tag::VIDEO_FRAME_RATE,    static_cast<double>(cfg_.fps));
    fmt.PutLongValue(Tag::MEDIA_BITRATE,         static_cast<int64_t>(cfg_.bitrate));
    fmt.PutIntValue(Tag::VIDEO_I_FRAME_INTERVAL, 0);     // 0 = 首帧强制 IDR
    // cvt 产品竖屏当横屏用，编码时标记旋转角度（客户端播放器会自动旋转显示）
    fmt.PutIntValue(Tag::VIDEO_ROTATION,         cfg_.rotation);

    ret = encoder_->Configure(fmt);
    if (ret != AVCS_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "Configure 失败: %{public}d", ret);
        encoder_->Release();
        encoder_.reset();
        return false;
    }

    // ★ Surface 输入模式：调用 CreateInputSurface 获取编码器消费端 Surface
    //   此后不再有 OnInputBufferAvailable 回调，帧由 Surface 生产/消费
    encoderSurface_ = encoder_->CreateInputSurface();
    if (encoderSurface_ == nullptr) {
        HILOG_ERROR(LOG_CORE, "CreateInputSurface 失败");
        encoder_->Release();
        encoder_.reset();
        return false;
    }

    ret = encoder_->Prepare();
    if (ret != AVCS_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "Prepare 失败: %{public}d", ret);
        encoder_->Release();
        encoder_.reset();
        encoderSurface_ = nullptr;
        return false;
    }

    ret = encoder_->Start();
    if (ret != AVCS_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "Start 失败: %{public}d", ret);
        encoder_->Release();
        encoder_.reset();
        encoderSurface_ = nullptr;
        return false;
    }

    HILOG_INFO(LOG_CORE, "C++ 编码器已启动（Surface 输入）%{public}dx%{public}d @%{public}dfps bitrate=%{public}d",
               cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate);
    return true;
}

void Capture::DestroyEncoder()
{
    if (encoder_ == nullptr) { return; }
    encoder_->Stop();
    encoder_->Release();
    encoder_.reset();
    encoderSurface_ = nullptr;
    HILOG_INFO(LOG_CORE, "C++ 编码器已销毁");
}

// 请求编码器立即输出一个 IDR 帧
void Capture::RequestIDRFrame()
{
    if (encoder_ == nullptr) { return; }
    Format fmt;
    fmt.PutIntValue(Tag::VIDEO_REQUEST_I_FRAME, 1);
    int32_t ret = encoder_->SetParameter(fmt);
    HILOG_INFO(LOG_CORE, "RequestIDRFrame ret=%{public}d", ret);
}

// ── 启动 / 停止 ──────────────────────────────────────────────────────────────
bool Capture::Start()
{
    if (running_.load()) { return true; }

    // 1. 初始化编码器
    if (!InitEncoder()) { return false; }

    // 2. 创建采集实例
    capture_ = ScreenCaptureFactory::CreateScreenCapture();
    if (capture_ == nullptr) {
        HILOG_ERROR(LOG_CORE, "ScreenCaptureFactory::CreateScreenCapture 失败");
        DestroyEncoder();
        return false;
    }

    // 3. 注册回调
    auto cb = std::make_shared<CaptureCallback>(this);
    int32_t ret = capture_->SetScreenCaptureCallback(cb);
    if (ret != SCREEN_CAPTURE_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "SetScreenCaptureCallback 失败: %{public}d", ret);
        capture_->Release();
        capture_.reset();
        DestroyEncoder();
        return false;
    }

    // 4. 配置采集参数
    //    dataType = ORIGINAL_STREAM：Surface 模式所需
    //    videoSource = VIDEO_SOURCE_SURFACE_RGBA：采集 RGBA 格式
    AVScreenCaptureConfig config{};
    config.captureMode = CaptureMode::CAPTURE_HOME_SCREEN;
    config.dataType    = DataType::ORIGINAL_STREAM;
    config.videoInfo.videoCapInfo.videoFrameWidth  = cfg_.width;
    config.videoInfo.videoCapInfo.videoFrameHeight = cfg_.height;
    config.videoInfo.videoCapInfo.videoSource      = VideoSourceType::VIDEO_SOURCE_SURFACE_RGBA;
    config.audioInfo.micCapInfo.audioSampleRate    = 0;
    config.audioInfo.micCapInfo.audioChannels      = 0;
    config.audioInfo.micCapInfo.audioSource        = AudioCaptureSourceType::SOURCE_DEFAULT;
    config.audioInfo.innerCapInfo.audioSampleRate  = 0;
    config.audioInfo.innerCapInfo.audioChannels    = 0;
    config.audioInfo.innerCapInfo.audioSource      = AudioCaptureSourceType::SOURCE_DEFAULT;
    // 让采集画布跟随屏幕旋转（修复物理竖屏当横屏使用时画面截断的问题）
    config.strategy.canvasFollowRotation = true;

    ret = capture_->Init(config);
    if (ret != SCREEN_CAPTURE_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "Capture::Init 失败: %{public}d", ret);
        capture_->Release();
        capture_.reset();
        DestroyEncoder();
        return false;
    }

    capture_->SetMicrophoneEnabled(false);

    // 5. 尝试设置帧率上限
    ret = capture_->SetMaxVideoFrameRate(cfg_.fps);
    if (ret != SCREEN_CAPTURE_ERR_OK) {
        HILOG_WARN(LOG_CORE, "SetMaxVideoFrameRate 失败: %{public}d（忽略）", ret);
    }

    // 6. 开始采集（Surface 模式：采集器直接往编码器 Surface 写帧，无需回调中转）
    ret = capture_->StartScreenCaptureWithSurface(encoderSurface_);
    if (ret != SCREEN_CAPTURE_ERR_OK) {
        HILOG_ERROR(LOG_CORE, "StartScreenCaptureWithSurface 失败: %{public}d", ret);
        capture_->Release();
        capture_.reset();
        DestroyEncoder();
        return false;
    }

    running_.store(true);

    // 7. 立即请求一个 IDR 帧，确保编码器最快产出关键帧
    RequestIDRFrame();

    HILOG_INFO(LOG_CORE, "屏幕采集+编码已启动 %{public}dx%{public}d @%{public}dfps",
               cfg_.width, cfg_.height, cfg_.fps);
    return true;
}

void Capture::Stop()
{
    if (!running_.exchange(false)) { return; }

    if (capture_ != nullptr) {
        capture_->StopScreenCapture();
        capture_->Release();
        capture_.reset();
    }

    DestroyEncoder();
    HILOG_INFO(LOG_CORE, "屏幕采集+编码已停止");
}

// ── CaptureCallback 实现 ─────────────────────────────────────────────────────

void Capture::CaptureCallback::OnError(
    ScreenCaptureErrorType /*errorType*/, int32_t errorCode)
{
    HILOG_ERROR(LOG_CORE, "采集错误: %{public}d", errorCode);
    if (owner_) { owner_->running_.store(false); }
}

void Capture::CaptureCallback::OnAudioBufferAvailable(
    bool /*isReady*/, AudioCaptureSourceType /*type*/)
{
    // 不采集音频，忽略
}

void Capture::CaptureCallback::OnStateChange(
    AVScreenCaptureStateCode stateCode)
{
    switch (stateCode) {
        case SCREEN_CAPTURE_STATE_STARTED:
            HILOG_INFO(LOG_CORE, "采集已开始");
            break;
        case SCREEN_CAPTURE_STATE_CANCELED:
            HILOG_WARN(LOG_CORE, "采集被用户取消");
            if (owner_) { owner_->running_.store(false); }
            break;
        case SCREEN_CAPTURE_STATE_STOPPED_BY_USER:
        case SCREEN_CAPTURE_STATE_STOPPED_BY_CALL:
            HILOG_INFO(LOG_CORE, "采集已停止");
            if (owner_) { owner_->running_.store(false); }
            break;
        case SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER:
            HILOG_WARN(LOG_CORE, "采集被中断");
            if (owner_) { owner_->running_.store(false); }
            break;
        default:
            break;
    }
}

void Capture::CaptureCallback::OnVideoBufferAvailable(bool isReady)
{
    // Surface 模式下，采集器直接写入编码器 Surface，此回调不再触发。
    // 保留空实现以满足接口要求。
    (void)isReady;
    if (owner_ == nullptr) { return; }
    HILOG_DEBUG(LOG_CORE, "OnVideoBufferAvailable called in surface mode (unexpected)");

}

// ── EncCallback 实现 ─────────────────────────────────────────────────────────

void Capture::EncCallback::OnError(AVCodecErrorType /*type*/,
                                                 int32_t errorCode)
{
    HILOG_ERROR(LOG_CORE, "编码器错误: %{public}d", errorCode);
    if (owner_) { owner_->running_.store(false); }
}

void Capture::EncCallback::OnOutputFormatChanged(const Format & /*fmt*/)
{
    HILOG_INFO(LOG_CORE, "编码器格式变化");
}

void Capture::EncCallback::OnOutputBufferAvailable(
    uint32_t index, std::shared_ptr<AVBuffer> buffer)
{
    if (owner_ == nullptr || buffer == nullptr) { return; }

    uint8_t *addr = (buffer->memory_ != nullptr) ? buffer->memory_->GetAddr() : nullptr;
    int32_t  size = (buffer->memory_ != nullptr) ? buffer->memory_->GetSize() : 0;

    if (addr != nullptr && size > 0 && owner_->cfg_.onNalu) {
        // 打印前 16 字节十六进制，确认数据格式（Annex-B vs AVCC）
        char hex[128] = {};
        int hp = 0;
        for (int i = 0; i < size && i < 16; i++) {
            hp += snprintf(hex + hp, sizeof(hex) - hp, "%02x ", addr[i]);
        }
        HILOG_INFO(LOG_CORE, "OnOutputBufferAvailable: size=%{public}d pts=%{public}lld flag=0x%{public}x hex=[%{public}s]",
                   size, static_cast<long long>(buffer->pts_), buffer->flag_, hex);

        // 判断是否 IDR 帧：
        //   Annex-B 格式: 00 00 00 01 XX, NALU type = XX & 0x1F, IDR=5
        //   AVCC 格式:    NN NN NN NN XX, 前4字节是长度，NALU type = XX & 0x1F
        bool isKey = false;
        int offset = 0;
        if (size >= 4 && addr[0] == 0x00 && addr[1] == 0x00 &&
            addr[2] == 0x00 && addr[3] == 0x01) {
            // Annex-B 格式
            offset = 4;
        } else if (size >= 3 && addr[0] == 0x00 && addr[1] == 0x00 &&
                   addr[2] == 0x01) {
            // Annex-B 格式（3字节起始码）
            offset = 3;
        } else {
            // 可能是 AVCC 格式（前4字节是长度），跳过
            offset = 4;
        }
        if (offset < size) {
            int naluType = addr[offset] & 0x1F;
            isKey = (naluType == 5);  // 5 = IDR slice
        }
        // 兜底：用 flag 判断
        if (!isKey) {
            isKey = (buffer->flag_ &
                static_cast<uint32_t>(AVCodecBufferFlag::AVCODEC_BUFFER_FLAG_SYNC_FRAME)) != 0;
        }
        owner_->cfg_.onNalu(addr, size, buffer->pts_, isKey);
    }

    if (owner_->encoder_ != nullptr) {
        owner_->encoder_->ReleaseOutputBuffer(index);
    }
}

} // namespace ohscrcpy
