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
 * @file input.cpp
 * @brief 输入控制服务实现
 *
 * 监听 TCP 控制端口，接收客户端的二进制输入控制协议消息，
 * 通过 InputManager::SimulateInputEvent 注入鼠标、触摸和键盘事件到 OpenHarmony 系统。
 * 依赖：libmmi-client（同 uinput 工具）
 *
 * 权限要求：ohos.permission.INJECT_INPUT_EVENT
 */

#include "input.h"

#include <cstring>
#include <cerrno>
#include <chrono>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

#include "hilog/log.h"

// InputManager C++ API（libmmi-client）
#include "input_manager.h"
#include "pointer_event.h"
#include "key_event.h"

#undef LOG_TAG
#define LOG_TAG "ohscrcpy_input"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD002B03

// 读取 int32_t（小端序）
static inline int32_t ReadLE32(const uint8_t *p)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(p[0]))       |
        (static_cast<uint32_t>(p[1]) <<  8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24));
}

// 消息类型定义（与 input.h 协议一致）
static constexpr uint8_t kMsgMouseMove      = 0x01;
static constexpr uint8_t kMsgMouseDown      = 0x02;
static constexpr uint8_t kMsgMouseUp        = 0x03;
static constexpr uint8_t kMsgMouseScroll    = 0x04;
static constexpr uint8_t kMsgTouchDown      = 0x11;
static constexpr uint8_t kMsgTouchMove      = 0x12;
static constexpr uint8_t kMsgTouchUp        = 0x13;
static constexpr uint8_t kMsgKeyDown        = 0x21;
static constexpr uint8_t kMsgKeyUp          = 0x22;

using namespace OHOS::MMI;

namespace ohscrcpy {

// ─── 构造 / 析构 ──────────────────────────────────────────────────────────────

InputServer::InputServer(int port, int screenW, int screenH,
                         bool useAbstract, const std::string &sockName)
    : port_(port), screenW_(screenW), screenH_(screenH),
      useAbstract_(useAbstract), sockName_(sockName)
{
}

InputServer::~InputServer()
{
    Stop();
}

// ─── Start / Stop ─────────────────────────────────────────────────────────────

bool InputServer::Start()
{
    if (running_.load()) {
        return true;
    }

    if (useAbstract_) {
        // ── Unix Domain Socket abstract 命名空间 ──
        listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            HILOG_ERROR(LOG_CORE, "创建控制 abstract socket 失败: %{public}s", strerror(errno));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, sockName_.c_str(), sizeof(addr.sun_path) - 2);
        socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + 1 + sockName_.size();

        if (bind(listenFd_, reinterpret_cast<struct sockaddr *>(&addr), addrLen) < 0) {
            HILOG_ERROR(LOG_CORE, "绑定控制 abstract socket %{public}s 失败: %{public}s",
                        sockName_.c_str(), strerror(errno));
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        if (listen(listenFd_, 5) < 0) {
            HILOG_ERROR(LOG_CORE, "abstract listen 失败: %{public}s", strerror(errno));
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        running_.store(true);
        acceptThread_ = std::thread(&InputServer::AcceptLoop, this);
        HILOG_INFO(LOG_CORE, "输入控制服务已启动（abstract: %{public}s）", sockName_.c_str());
        return true;
    }

    // ── TCP 模式 ──
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "创建控制端口 socket 失败");
        return false;
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (bind(listenFd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        HILOG_ERROR(LOG_CORE, "绑定控制端口 %{public}d 失败: %{public}s",
                    port_, strerror(errno));
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (listen(listenFd_, 5) < 0) {
        HILOG_ERROR(LOG_CORE, "listen 失败");
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_.store(true);
    acceptThread_ = std::thread(&InputServer::AcceptLoop, this);

    HILOG_INFO(LOG_CORE, "输入控制服务已启动，端口 %{public}d", port_);
    return true;
}

void InputServer::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (listenFd_ >= 0) {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    HILOG_INFO(LOG_CORE, "输入控制服务已停止");
}

void InputServer::SetTargetDisplayId(int32_t displayId)
{
    // 只在运行期被调用一次（虚拟屏建好之后），写 int32 本身是原子的
    targetDisplayId_.store(displayId);
    HILOG_INFO(LOG_CORE, "注入目标屏幕 displayId=%{public}d（-1 表示由 MMI 推断）", displayId);
}

void InputServer::ApplyTargetDisplay(InputEvent *event)
{
    if (event == nullptr) {
        return;
    }
    int32_t displayId = targetDisplayId_.load();
    if (displayId >= 0) {
        // 显式指定目标屏，避免多屏（物理屏/遗留虚拟屏）时注入落到别的屏上
        event->SetTargetDisplayId(displayId);
    }
}

// ─── AcceptLoop ───────────────────────────────────────────────────────────────

void InputServer::AcceptLoop()
{
    while (running_.load()) {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = accept(listenFd_,
                              reinterpret_cast<struct sockaddr *>(&clientAddr),
                              &addrLen);
        if (clientFd < 0) {
            if (!running_.load()) {
                break;
            }
            continue;
        }

        char ipBuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        HILOG_INFO(LOG_CORE, "控制客户端连接: %{public}s", ipBuf);

        // 每个控制客户端独立线程（一般只有一个控制端）
        std::thread([this, clientFd]() {
            HandleClient(clientFd);
            close(clientFd);
        }).detach();
    }
}

// ─── HandleClient ─────────────────────────────────────────────────────────────

void InputServer::HandleClient(int fd)
{
    while (running_.load()) {
        // 读取消息类型（1 字节）
        uint8_t type = 0;
        if (!ReadExact(fd, &type, 1)) {
            break;
        }
        ProcessMsg(type, fd);
    }
    HILOG_INFO(LOG_CORE, "控制客户端断开");
}

// ─── ProcessMsg ───────────────────────────────────────────────────────────────

bool InputServer::ReadExact(int fd, uint8_t *buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(fd, buf + received, len - received, 0);
        if (n <= 0) {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

void InputServer::ProcessMsg(uint8_t type, int fd)
{
    uint8_t buf[16] = {};

    switch (type) {
        case kMsgMouseMove: {
            // [int32_t x][int32_t y]  8 字节
            if (!ReadExact(fd, buf, 8)) { return; }
            int32_t x = ReadLE32(buf);
            int32_t y = ReadLE32(buf + 4);
            HILOG_DEBUG(LOG_CORE, "MouseMove recv: x=%{public}d y=%{public}d", x, y);
            InjectMouseMove(x, y);
            HILOG_DEBUG(LOG_CORE, "MouseMove inject done");
            break;
        }
        case kMsgMouseDown:
        case kMsgMouseUp: {
            // [int32_t x][int32_t y][uint8_t button]  9 字节
            HILOG_INFO(LOG_CORE, "Mouse%{public}s recv start",
                (type == kMsgMouseDown) ? "Down" : "Up");
            if (!ReadExact(fd, buf, 9)) { return; }
            int32_t x   = ReadLE32(buf);
            int32_t y   = ReadLE32(buf + 4);
            uint8_t btn = buf[8];
            HILOG_INFO(LOG_CORE,
                "Mouse%{public}s read done: x=%{public}d y=%{public}d btn=%{public}u",
                (type == kMsgMouseDown) ? "Down" : "Up", x, y, static_cast<unsigned>(btn));
            int32_t action = (type == kMsgMouseDown)
                             ? PointerEvent::POINTER_ACTION_BUTTON_DOWN
                             : PointerEvent::POINTER_ACTION_BUTTON_UP;
            InjectMouseButton(x, y, btn, action);
            HILOG_INFO(LOG_CORE, "Mouse%{public}s inject done",
                (type == kMsgMouseDown) ? "Down" : "Up");
            break;
        }
        case kMsgMouseScroll: {
            // [int32_t x][int32_t y][int32_t delta]  12 字节
            if (!ReadExact(fd, buf, 12)) { return; }
            int32_t x     = ReadLE32(buf);
            int32_t y     = ReadLE32(buf + 4);
            int32_t delta = ReadLE32(buf + 8);
            InjectMouseScroll(x, y, delta);
            break;
        }
        case kMsgTouchDown:
        case kMsgTouchMove:
        case kMsgTouchUp: {
            // [uint8_t fingerId][int32_t x][int32_t y]  9 字节
            if (!ReadExact(fd, buf, 9)) { return; }
            uint8_t fingerId = buf[0];
            int32_t x = ReadLE32(buf + 1);
            int32_t y = ReadLE32(buf + 5);
            int32_t action;
            if (type == kMsgTouchDown)      { action = PointerEvent::POINTER_ACTION_DOWN; }
            else if (type == kMsgTouchMove) { action = PointerEvent::POINTER_ACTION_MOVE; }
            else                            { action = PointerEvent::POINTER_ACTION_UP;   }
            InjectTouch(action, fingerId, x, y);
            break;
        }
        case kMsgKeyDown:
        case kMsgKeyUp: {
            // [int32_t keyCode]  4 字节
            if (!ReadExact(fd, buf, 4)) { return; }
            int32_t keyCode = ReadLE32(buf);
            int32_t action = (type == kMsgKeyDown)
                             ? KeyEvent::KEY_ACTION_DOWN
                             : KeyEvent::KEY_ACTION_UP;
            InjectKey(action, keyCode);
            break;
        }
        default:
            HILOG_WARN(LOG_CORE, "未知控制消息类型: 0x%{public}02x", type);
            break;
    }
}

// ─── InputManager 注入实现 ───────────────────────────────────────────────────

static int64_t NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int32_t InputServer::MapX(int32_t x) const
{
    // TODO: 若客户端坐标系与屏幕不同，可在此做缩放：x * screenW_ / clientW_
    (void)screenW_;
    return x;
}

int32_t InputServer::MapY(int32_t y) const
{
    (void)screenH_;
    return y;
}

void InputServer::InjectMouseMove(int32_t x, int32_t y)
{
    // 鼠标移动映射为触摸移动（OH 设备通常没有鼠标驱动）
    auto event = PointerEvent::Create();
    if (!event) { return; }

    PointerEvent::PointerItem item;
    item.SetPointerId(0);
    item.SetDisplayX(MapX(x));
    item.SetDisplayY(MapY(y));
    item.SetPressed(true);

    event->SetPointerId(0);
    event->AddPointerItem(item);
    event->SetPointerAction(PointerEvent::POINTER_ACTION_MOVE);
    event->SetSourceType(PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
    event->SetActionTime(NowUs());
    ApplyTargetDisplay(event.get());

    InputManager::GetInstance()->SimulateInputEvent(event);
}

void InputServer::InjectMouseButton(int32_t x, int32_t y, uint8_t button, int32_t action)
{
    // 鼠标按下/松开映射为触摸 DOWN/UP
    HILOG_INFO(LOG_CORE, "InjectMouseButton start: x=%{public}d y=%{public}d btn=%{public}u",
        x, y, static_cast<unsigned>(button));
    
    auto event = PointerEvent::Create();
    if (!event) {
        HILOG_ERROR(LOG_CORE, "InjectMouseButton: PointerEvent::Create 返回 nullptr");
        return;
    }
    HILOG_DEBUG(LOG_CORE, "PointerEvent created");

    int32_t touchAction = (action == PointerEvent::POINTER_ACTION_BUTTON_DOWN)
                          ? PointerEvent::POINTER_ACTION_DOWN
                          : PointerEvent::POINTER_ACTION_UP;

    PointerEvent::PointerItem item;
    item.SetPointerId(0);
    item.SetDisplayX(MapX(x));
    item.SetDisplayY(MapY(y));
    item.SetPressed(touchAction == PointerEvent::POINTER_ACTION_DOWN);

    event->SetPointerId(0);
    event->AddPointerItem(item);
    event->SetPointerAction(touchAction);
    event->SetSourceType(PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
    event->SetActionTime(NowUs());
    ApplyTargetDisplay(event.get());
    HILOG_DEBUG(LOG_CORE, "PointerEvent built, calling SimulateInputEvent");

    InputManager::GetInstance()->SimulateInputEvent(event);
    HILOG_INFO(LOG_CORE, "InjectMouseButton SimulateInputEvent returned");
}

void InputServer::InjectMouseScroll(int32_t x, int32_t y, int32_t delta)
{
    auto event = PointerEvent::Create();
    if (!event) { return; }

    PointerEvent::PointerItem item;
    item.SetPointerId(0);
    item.SetDisplayX(MapX(x));
    item.SetDisplayY(MapY(y));
    item.SetPressed(false);

    event->SetPointerId(0);
    event->AddPointerItem(item);
    event->SetPointerAction(PointerEvent::POINTER_ACTION_AXIS_UPDATE);
    event->SetSourceType(PointerEvent::SOURCE_TYPE_MOUSE);
    event->SetAxisValue(PointerEvent::AXIS_TYPE_SCROLL_VERTICAL, static_cast<double>(delta));
    event->SetActionTime(NowUs());
    ApplyTargetDisplay(event.get());

    InputManager::GetInstance()->SimulateInputEvent(event);
}

void InputServer::InjectTouch(int32_t action, uint8_t fingerId, int32_t x, int32_t y)
{
    auto event = PointerEvent::Create();
    if (!event) { return; }

    PointerEvent::PointerItem item;
    item.SetPointerId(static_cast<int32_t>(fingerId));
    item.SetDisplayX(MapX(x));
    item.SetDisplayY(MapY(y));
    item.SetPressed(action != PointerEvent::POINTER_ACTION_UP);

    event->SetPointerId(static_cast<int32_t>(fingerId));
    event->AddPointerItem(item);
    event->SetPointerAction(action);
    event->SetSourceType(PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
    event->SetActionTime(NowUs());
    ApplyTargetDisplay(event.get());

    InputManager::GetInstance()->SimulateInputEvent(event);
}

void InputServer::InjectKey(int32_t action, int32_t keyCode)
{
    auto event = KeyEvent::Create();
    if (!event) { return; }

    KeyEvent::KeyItem item;
    item.SetKeyCode(keyCode);
    item.SetDownTime(NowUs());
    item.SetPressed(action == KeyEvent::KEY_ACTION_DOWN);

    event->SetKeyAction(action);
    event->AddKeyItem(item);
    event->SetKeyCode(keyCode);
    ApplyTargetDisplay(event.get());

    InputManager::GetInstance()->SimulateInputEvent(event);
}

} // namespace ohscrcpy
