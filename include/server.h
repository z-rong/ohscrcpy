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

#ifndef OHSCRCPY_SERVER_H
#define OHSCRCPY_SERVER_H

#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <string>
#include <cstring>

namespace ohscrcpy {

/**
 * @brief H.264 NALU 视频流服务器（TCP 或 Unix Domain Socket）
 *
 * 支持两种监听模式：
 *   1. TCP 端口（默认）：监听 INADDR_ANY:port，适合网络直连
 *   2. Unix Domain Socket abstract 命名空间：监听 "\0ohscrcpy_video"，
 *      配合 hdc fport localabstract 使用，绕过 TCP 协议栈，延迟更低
 *
 * 帧协议（小端序）：
 *   [magic:4]     固定 0x4F485343 ("OHSC")
 *   [flags:1]     bit0 = isKey（关键帧标志）
 *   [pts:8]       时间戳（微秒，int64）
 *   [size:4]      NALU 数据字节数
 *   [width:4]     视频宽度（像素，编码帧方向）
 *   [height:4]    视频高度（像素，编码帧方向）
 *   [rotation:4]  画面相对正立方向的旋转角（0/90/180/270）。
 *                 竖屏物理面板当横屏用的设备（cvt 等）按物理面板方向编码，
 *                 帧内容是旋转过的，客户端按该角度转正显示
 *   [data]        H.264 NALU 数据
 */
class Server {
public:
    /**
     * @param port       TCP 监听端口（useAbstract=false 时有效）
     * @param width      视频宽度
     * @param height     视频高度
     * @param useAbstract 是否使用 Unix Domain Socket abstract 命名空间
     * @param sockName   abstract socket 名称（默认 "ohscrcpy_video"）
     */
    Server(int port, int32_t width = 0, int32_t height = 0,
               bool useAbstract = false, const std::string &sockName = "ohscrcpy_video");
    ~Server();

    /** 启动监听，成功返回 true */
    bool Start();
    /** 停止并关闭所有连接 */
    void Stop();

    /**
     * @brief 推入一个 H.264 NALU
     * @param data   NALU 数据地址
     * @param size   NALU 字节数
     * @param pts    时间戳（微秒）
     * @param isKey  是否关键帧（IDR）
     */
    void PushNalu(const uint8_t *data, int32_t size, int64_t pts, bool isKey);

    /** 更新视频分辨率（每帧帧头携带，客户端自动适配窗口大小）*/
    void SetResolution(int32_t width, int32_t height) { width_ = width; height_ = height; }

    /** 设置画面旋转角度（0/90/180/270），随每帧帧头下发给客户端 */
    void SetRotation(int32_t rotation) { rotation_ = rotation; }

    /**
     * @brief 设置新客户端连接回调，用于触发 IDR 帧请求
     * 每次有新客户端成功连接时调用一次 cb()
     */
    void SetNewClientCallback(std::function<void()> cb) { newClientCb_ = std::move(cb); }

private:
    void AcceptLoop();

    int          port_;
    int          listenFd_ = -1;
    bool         useAbstract_ = false;
    std::string  sockName_;

    std::atomic<bool>   running_{false};
    std::thread         acceptThread_;

    std::mutex          clientsMutex_;
    std::vector<int>    clients_;         // 已收到首个 IDR 的客户端 fd
    std::vector<int>    pendingClients_;  // 新连接，等待 IDR 帧后升级

    std::function<void()> newClientCb_; // 新客户端连接时触发（用于请求 IDR 帧）

    // 缓存最近一个完整的 IDR 帧（含帧头），新客户端连接时立即补发
    struct CachedFrame {
        std::vector<uint8_t> hdr;   // NaluHeader（29 字节）
        std::vector<uint8_t> data;  // NALU 数据
    };
    std::mutex              idrMutex_;
    CachedFrame             idrCache_; // 有效当 !data.empty()

    // SPS/PPS 缓存：新客户端连接时必须先收到 SPS+PPS 才能解码 IDR
    std::mutex              spsPpsMutex_;
    std::vector<uint8_t>    spsData_;  // SPS NALU（含起始码）
    std::vector<uint8_t>    ppsData_;  // PPS NALU（含起始码）

    int32_t  width_  = 0;  // 视频宽度，随每帧帧头下发
    int32_t  height_ = 0;  // 视频高度，随每帧帧头下发
    int32_t  rotation_ = 0; // 画面旋转角（0/90/180/270），随每帧帧头下发

    static constexpr uint32_t kMagic = 0x4F485343u; // "OHSC"
};

} // namespace ohscrcpy

using Server = ohscrcpy::Server;

#endif // OHSCRCPY_SERVER_H
