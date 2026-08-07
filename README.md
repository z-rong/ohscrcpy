# ohscrcpy — OpenHarmony 投屏工具

轻量级 OpenHarmony 屏幕镜像 + 输入注入工具，类似 Android 的 scrcpy。

## 架构

```
┌──────────────────────────────────────────────────┐
│  设备端 (ohscrcpy)                                │
│                                                  │
│  ScreenCapture ──H.264 NALU──► Server (TCP)      │
│                                   ↕              │
│  InputServer (TCP) ◄── 输入协议 ── 客户端         │
└──────────────────────────────────────────────────┘
         ↕  hdc fport (abstract unix socket)
┌──────────────────────────────────────────────────┐
│  PC 客户端 (ohscrcpy_client.py)                   │
│                                                  │
│  TCP 收 NALU → ffmpeg 解码 → pygame 渲染          │
│  pygame 事件 → 输入协议 → TCP 发送                │
└──────────────────────────────────────────────────┘
```

## 编译（服务端）

### 前提

- OpenHarmony 全量源码环境（已完成 prebuilts 下载）
- 已配置好产品编译环境（如 rk3568 ）
- 将本仓克隆到foundation/multimedia/player_framework/test/目录下

### 单独编译 ohscrcpy 目标

```bash
# 在 OH 源码根目录执行
./build.sh --product-name rk3568 --build-target ohscrcpy 
```


## 客户端依赖

PC 端（Windows）需要：

```bash
pip install pygame opencv-python numpy
```

另外需要系统安装 **ffmpeg** 并确保在 PATH 中。

## 使用方法

将编译好的 `ohscrcpy` 二进制放到 `client/` 目录下，然后：

```bash
cd foundation/multimedia/player_framework/test/ohscrcpy/client

# 单设备
python ohscrcpy_client.py

# 多设备时指定
python ohscrcpy_client.py -t <device_id>

# 详细日志
python ohscrcpy_client.py -t <device_id> -v
```

客户端会自动完成：推送二进制 → 端口转发 → 启动服务端 → 连接 → 显示画面。
退出时自动清理（杀进程、删文件、移除转发）。

### 4. 客户端操作

| 操作 | 功能 |
|------|------|
| 鼠标左键点击/拖动 | 触摸注入 |
| 鼠标滚轮 | 滚动注入 |
| `h` 键 | Home 键 |
| `Esc` 键 | 退出客户端 |

## 文件说明

| 文件 | 说明 |
|------|------|
| `main.cpp` | 入口，参数解析，组件编排 |
| `screen_capture.cpp/h` | 屏幕采集 + H.264 编码（Surface 模式） |
| `server.cpp/h` | TCP 帧流服务器，发送 NALU 给客户端 |
| `input.cpp/h` | 输入注入服务，接收鼠标/触摸/按键事件 |
| `BUILD.gn` | GN 构建脚本 |
| `client/ohscrcpy_client.py` | Python 客户端（pygame + ffmpeg） |

