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

#ifndef OHSCRCPY_INPUT_H
#define OHSCRCPY_INPUT_H

#include <cstdint>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace OHOS {
namespace MMI {
class InputEvent;
} // namespace MMI
} // namespace OHOS

namespace ohscrcpy {

/**
 * @brief 输入控制服务
 *
 * 监听 TCP 控制端口，接收客户端发来的输入控制命令，
 * 调用 OH_Input_InjectMouseEvent / OH_Input_InjectTouchEvent 注入到系统。
 *
 * ── 控制协议（二进制，小端序）──────────────────────────────────────────────
 *
 * 所有消息以 1 字节 type 开头，后接固定长度的结构体。
 *
 * type=0x01  鼠标移动（MOUSE_ACTION_MOVE）
 *   struct { uint8_t type; int32_t x; int32_t y; }  共 9 字节
 *
 * type=0x02  鼠标按下（MOUSE_ACTION_BUTTON_DOWN）
 *   struct { uint8_t type; int32_t x; int32_t y; uint8_t button; }  共 10 字节
 *   button: 0=LEFT, 1=MIDDLE, 2=RIGHT
 *
 * type=0x03  鼠标释放（MOUSE_ACTION_BUTTON_UP）
 *   struct { uint8_t type; int32_t x; int32_t y; uint8_t button; }  共 10 字节
 *
 * type=0x04  鼠标滚轮
 *   struct { uint8_t type; int32_t x; int32_t y; int32_t delta; }  共 13 字节
 *   delta > 0 向上滚，< 0 向下滚
 *
 * type=0x11  触摸按下（TOUCH_ACTION_DOWN）
 *   struct { uint8_t type; uint8_t fingerId; int32_t x; int32_t y; }  共 10 字节
 *
 * type=0x12  触摸移动（TOUCH_ACTION_MOVE）
 *   struct { uint8_t type; uint8_t fingerId; int32_t x; int32_t y; }  共 10 字节
 *
 * type=0x13  触摸释放（TOUCH_ACTION_UP）
 *   struct { uint8_t type; uint8_t fingerId; int32_t x; int32_t y; }  共 10 字节
 *
 * type=0x21  按键按下（KEY_ACTION_DOWN）
 *   struct { uint8_t type; int32_t keyCode; }  共 5 字节
 *
 * type=0x22  按键释放（KEY_ACTION_UP）
 *   struct { uint8_t type; int32_t keyCode; }  共 5 字节
 *
 * 坐标使用屏幕分辨率坐标（与投屏视频分辨率一致，服务端会自动映射到实际屏幕）。
 * ──────────────────────────────────────────────────────────────────────────────
 */
class InputServer {
public:
    /**
     * @param port       TCP 监听端口（useAbstract=false 时有效）
     * @param screenW    投屏视频宽度（用于坐标映射）
     * @param screenH    投屏视频高度
     * @param useAbstract 是否使用 Unix Domain Socket abstract 命名空间
     * @param sockName   abstract socket 名称（默认 "ohscrcpy_ctrl"）
     */
    InputServer(int port, int screenW, int screenH,
               bool useAbstract = false, const std::string &sockName = "ohscrcpy_ctrl");
    ~InputServer();

    bool Start();
    void Stop();

    /**
     * @brief 指定注入事件的目标屏幕（displayId）
     *
     * 不设置时事件的目标屏幕由 MMI 用「默认显示组里第一个屏幕」推断（targetDisplayId = -1）。
     * 无屏设备上这个推断恰好命中我们创建的虚拟屏，但一旦系统里同时存在多块屏
     *（物理屏 + 虚拟屏、或遗留的虚拟屏），推断结果就可能不是我们正在编码的那块屏，
     * 表现为「画面能看、点按没反应」。所以这里显式指定，保证注入落到正确的屏上。
     * 传 -1 表示沿用 MMI 的默认推断。
     */
    void SetTargetDisplayId(int32_t displayId);

private:
    void AcceptLoop();
    void HandleClient(int fd);

    // 消息处理
    bool ReadExact(int fd, uint8_t *buf, size_t len);
    void ProcessMsg(uint8_t type, int fd);

    // InputManager 注入（同 uinput 工具一致）
    void InjectMouseMove  (int32_t x, int32_t y);
    void InjectMouseButton(int32_t x, int32_t y, uint8_t button, int32_t action);
    void InjectMouseScroll(int32_t x, int32_t y, int32_t delta);
    void InjectTouch      (int32_t action, uint8_t fingerId, int32_t x, int32_t y);
    // 维护按住键状态：DOWN 事件携带全部按住键的 KeyItem（组合键判定依据），
    // UP 事件只携带被释放的键；修饰键由客户端按 PC 修饰状态发 DOWN/UP 序列
    void InjectKey        (int32_t action, int32_t keyCode);

    // 当前按住的键（注入 DOWN 时随事件携带，UP 时移除）
    std::vector<int32_t> pressedKeys_;

    // 给注入事件设置目标屏幕（targetDisplayId_ >= 0 时生效）
    void ApplyTargetDisplay(OHOS::MMI::InputEvent *event);

    // 坐标从视频分辨率映射到实际屏幕分辨率
    int32_t MapX(int32_t x) const;
    int32_t MapY(int32_t y) const;

    int port_;
    int screenW_;
    int screenH_;
    int listenFd_ = -1;
    bool         useAbstract_ = false;
    std::string  sockName_;
    // 注入目标屏幕 id，-1 表示交给 MMI 推断
    std::atomic<int32_t> targetDisplayId_{-1};

    std::atomic<bool> running_{false};
    std::thread       acceptThread_;
};

} // namespace ohscrcpy

using InputServer = ohscrcpy::InputServer;

#endif // OHSCRCPY_INPUT_H
