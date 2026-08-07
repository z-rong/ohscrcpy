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
 * @file server.cpp
 * @brief TCP H.264 NALU 流服务器
 *
 * 监听 TCP 端口，将 H.264 NALU 以自定义二进制协议推送给所有连接的客户端。
 * 帧协议（小端序）：magic(4) + flags(1) + pts(8) + size(4) + data
 *   flags bit0: isKey
 */

#include "server.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "hilog/log.h"

#undef  LOG_TAG
#define LOG_TAG    "ohscrcpy_stream"
#undef  LOG_DOMAIN
#define LOG_DOMAIN 0xD002B01

namespace ohscrcpy {

Server::Server(int port, int32_t width, int32_t height,
                       bool useAbstract, const std::string &sockName)
    : port_(port), useAbstract_(useAbstract), sockName_(sockName),
      width_(width), height_(height)
{
}

Server::~Server()
{
    Stop();
}

bool Server::Start()
{
    if (useAbstract_) {
        // ── Unix Domain Socket abstract 命名空间 ──
        listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            HILOG_ERROR(LOG_CORE, "abstract socket 创建失败: %{public}s", strerror(errno));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        // abstract 命名空间：sun_path[0] = '\0'，后面跟名字
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, sockName_.c_str(), sizeof(addr.sun_path) - 2);
        socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + 1 + sockName_.size();

        if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), addrLen) < 0) {
            HILOG_ERROR(LOG_CORE, "abstract bind 失败: %{public}s", strerror(errno));
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        if (listen(listenFd_, 8) < 0) {
            HILOG_ERROR(LOG_CORE, "abstract listen 失败: %{public}s", strerror(errno));
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        running_ = true;
        acceptThread_ = std::thread(&Server::AcceptLoop, this);
        HILOG_INFO(LOG_CORE, "帧流服务器已启动（abstract: %{public}s）", sockName_.c_str());
        return true;
    }

    // ── TCP 模式 ──
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "socket 创建失败: %{public}s", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        HILOG_ERROR(LOG_CORE, "bind 失败: %{public}s", strerror(errno));
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (listen(listenFd_, 8) < 0) {
        HILOG_ERROR(LOG_CORE, "listen 失败: %{public}s", strerror(errno));
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&Server::AcceptLoop, this);
    HILOG_INFO(LOG_CORE, "帧流服务器已启动，端口 %{public}d", port_);
    return true;
}

void Server::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (int fd : clients_) {
        close(fd);
    }
    clients_.clear();
    HILOG_INFO(LOG_CORE, "帧流服务器已停止");
}

void Server::AcceptLoop()
{
    while (running_) {
        struct sockaddr_storage clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int fd = accept(listenFd_, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
        if (fd < 0) {
            if (running_) {
                HILOG_WARN(LOG_CORE, "accept 失败: %{public}s", strerror(errno));
            }
            break;
        }

        if (!useAbstract_) {
            // TCP 模式：关闭 Nagle，减少延迟
            int flag = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            char ipStr[INET_ADDRSTRLEN] = {};
            auto *sin = reinterpret_cast<sockaddr_in *>(&clientAddr);
            inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
            HILOG_INFO(LOG_CORE, "客户端连接: %{public}s", ipStr);
        } else {
            HILOG_INFO(LOG_CORE, "客户端连接（abstract socket）");
        }

        // 新客户端先放 pending 列表，等 IDR 帧到来再升级为正式客户端
        // 这样不会向新客户端推送 IDR 之前的非关键帧
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            pendingClients_.push_back(fd);
        }

        // 通知外部请求一个新 IDR 帧（编码器会尽快输出 IDR）
        if (newClientCb_) {
            newClientCb_();
        }
    }
}

// NALU 帧头结构（小端序，共 25 字节）
struct NaluHeader {
    uint32_t magic;  // 0x4F485343 ("OHSC")
    uint8_t  flags;  // bit0 = isKey
    int64_t  pts;    // 微秒
    int32_t  size;   // NALU 数据字节数
    int32_t  width;  // 视频宽度（像素）
    int32_t  height; // 视频高度（像素）
} __attribute__((packed));

// 辅助：从 Annex-B NALU 数据中解析 NALU type（跳过起始码）
static int ParseNaluType(const uint8_t *data, int32_t size)
{
    if (data == nullptr || size < 5) { return -1; }
    int offset = 0;
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        offset = 4;
    } else if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        offset = 3;
    }
    if (offset >= size) { return -1; }
    return data[offset] & 0x1F;
}

// 辅助：向 fd 发送一个完整的 NALU 帧（帧头+数据）
static bool SendFrame(int fd, const NaluHeader &hdr, const uint8_t *data, int32_t size)
{
    ssize_t s = send(fd, &hdr, sizeof(hdr), MSG_NOSIGNAL);
    if (s != static_cast<ssize_t>(sizeof(hdr))) { return false; }
    const uint8_t *p = data;
    ssize_t rem = size;
    while (rem > 0) {
        ssize_t n = send(fd, p, static_cast<size_t>(rem), MSG_NOSIGNAL);
        if (n <= 0) { return false; }
        p += n; rem -= n;
    }
    return true;
}

void Server::PushNalu(const uint8_t *data, int32_t size, int64_t pts, bool isKey)
{
    if (data == nullptr || size <= 0) {
        HILOG_WARN(LOG_CORE, "PushNalu 跳过: data=null或size=%{public}d", size);
        return;
    }

    // 解析 NALU type，检测 SPS/PPS
    int naluType = ParseNaluType(data, size);
    if (naluType == 7) {
        // SPS
        std::lock_guard<std::mutex> lk(spsPpsMutex_);
        spsData_.assign(data, data + size);
        // 打印完整 SPS 内容，确认是否包含 PPS
        char spsHex[256] = {};
        int shp = 0;
        for (int i = 0; i < size && i < 80; i++) {
            shp += snprintf(spsHex + shp, sizeof(spsHex) - shp, "%02x ", data[i]);
        }
        HILOG_INFO(LOG_CORE, "PushNalu: 缓存 SPS size=%{public}d hex=[%{public}s]", size, spsHex);
    } else if (naluType == 8) {
        // PPS
        std::lock_guard<std::mutex> lk(spsPpsMutex_);
        ppsData_.assign(data, data + size);
        HILOG_INFO(LOG_CORE, "PushNalu: 缓存 PPS size=%{public}d", size);
    }

    NaluHeader hdr{};
    hdr.magic  = kMagic;
    hdr.flags  = isKey ? 0x01u : 0x00u;
    hdr.pts    = pts;
    hdr.size   = size;
    hdr.width  = width_;
    hdr.height = height_;

    // IDR 帧缓存（含帧头），新客户端连接时立即补发
    if (isKey) {
        std::lock_guard<std::mutex> idrLock(idrMutex_);
        idrCache_.hdr.assign(reinterpret_cast<const uint8_t*>(&hdr),
                             reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
        idrCache_.data.assign(data, data + size);
    }

    std::lock_guard<std::mutex> lock(clientsMutex_);

    HILOG_INFO(LOG_CORE, "PushNalu: size=%{public}d key=%{public}d type=%{public}d clients=%{public}zu pending=%{public}zu",
               size, isKey ? 1 : 0, naluType, clients_.size(), pendingClients_.size());

    // IDR 帧到来时，将 pending 客户端升级为正式客户端
    // 关键：先发 SPS+PPS，再发 IDR 帧，否则解码器无法初始化
    if (isKey && !pendingClients_.empty()) {
        // 取 SPS/PPS 缓存
        std::vector<uint8_t> sps, pps;
        {
            std::lock_guard<std::mutex> lk(spsPpsMutex_);
            sps = spsData_;
            pps = ppsData_;
        }

        for (int fd : pendingClients_) {
            bool ok = true;

            // 1. 先发 SPS
            if (!sps.empty()) {
                NaluHeader spsHdr{};
                spsHdr.magic  = kMagic;
                spsHdr.flags  = 0x00;
                spsHdr.pts    = pts;
                spsHdr.size   = static_cast<int32_t>(sps.size());
                spsHdr.width  = width_;
                spsHdr.height = height_;
                ok = SendFrame(fd, spsHdr, sps.data(), static_cast<int32_t>(sps.size()));
                HILOG_INFO(LOG_CORE, "pending升级: 发送 SPS size=%{public}zu ok=%{public}d fd=%{public}d",
                           sps.size(), ok ? 1 : 0, fd);
            }

            // 2. 再发 PPS
            if (ok && !pps.empty()) {
                NaluHeader ppsHdr{};
                ppsHdr.magic  = kMagic;
                ppsHdr.flags  = 0x00;
                ppsHdr.pts    = pts;
                ppsHdr.size   = static_cast<int32_t>(pps.size());
                ppsHdr.width  = width_;
                ppsHdr.height = height_;
                ok = SendFrame(fd, ppsHdr, pps.data(), static_cast<int32_t>(pps.size()));
                HILOG_INFO(LOG_CORE, "pending升级: 发送 PPS size=%{public}zu ok=%{public}d fd=%{public}d",
                           pps.size(), ok ? 1 : 0, fd);
            }

            // 3. 最后发 IDR 帧
            if (ok) {
                ok = SendFrame(fd, hdr, data, size);
                HILOG_INFO(LOG_CORE, "pending升级: 发送 IDR size=%{public}d ok=%{public}d fd=%{public}d",
                           size, ok ? 1 : 0, fd);
            }

            if (ok) {
                clients_.push_back(fd);
                HILOG_INFO(LOG_CORE, "pending 客户端升级成功 fd=%{public}d", fd);
            } else {
                close(fd);
                HILOG_WARN(LOG_CORE, "pending 客户端升级失败 fd=%{public}d errno=%{public}d", fd, errno);
            }
        }
        pendingClients_.clear();
    }
    std::vector<int> dead;

    for (int fd : clients_) {
        // 发送帧头
        ssize_t sent = send(fd, &hdr, sizeof(hdr), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(sizeof(hdr))) {
            HILOG_WARN(LOG_CORE, "send帧头失败 ret=%{public}zd errno=%{public}d fd=%{public}d",
                       sent, errno, fd);
            dead.push_back(fd);
            continue;
        }
        // 发送 NALU 数据
        ssize_t remaining = size;
        const uint8_t *ptr = data;
        ssize_t dataSent = 0;
        while (remaining > 0) {
            ssize_t n = send(fd, ptr, static_cast<size_t>(remaining), MSG_NOSIGNAL);
            if (n <= 0) {
                HILOG_WARN(LOG_CORE, "send数据失败 ret=%{public}zd errno=%{public}d fd=%{public}d",
                           n, errno, fd);
                dead.push_back(fd);
                break;
            }
            ptr       += n;
            remaining -= n;
            dataSent  += n;
        }
        // 前5帧和每100帧打印一次发送结果
        static int sendLogCount = 0;
        if (sendLogCount < 5 || sendLogCount % 100 == 0) {
            HILOG_INFO(LOG_CORE, "send成功 fd=%{public}d 帧头=%{public}zd 数据=%{public}zd size=%{public}d (#%{public}d)",
                       fd, sent, dataSent, size, sendLogCount);
        }
        sendLogCount++;
    }

    // 移除断开的客户端
    for (int fd : dead) {
        HILOG_INFO(LOG_CORE, "客户端断开，fd=%{public}d", fd);
        close(fd);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
    }

    // 同时清理 pending 中已断开的连接
    pendingClients_.erase(
        std::remove_if(pendingClients_.begin(), pendingClients_.end(), [](int fd) {
            // 用 send 0 字节探活
            return send(fd, nullptr, 0, MSG_NOSIGNAL) < 0 && errno == EPIPE;
        }),
        pendingClients_.end());
}

} // namespace ohscrcpy
