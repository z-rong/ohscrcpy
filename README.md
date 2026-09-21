# ohscrcpy — OpenHarmony 投屏工具

轻量级 OpenHarmony 屏幕镜像 + 输入注入工具，类似 Android 的 scrcpy。

## 架构

```
┌──────────────────────────────────────────────────┐
│  设备端 (ohscrcpy)                                │
│                                                  │
│  Capture ──H.264 NALU──► Server (TCP)            │
│                                   ↕              │
│  InputServer (TCP) ◄── 输入协议 ── 客户端         │
└──────────────────────────────────────────────────┘
         ↕  hdc fport (abstract unix socket)
┌──────────────────────────────────────────────────┐
│  PC 客户端 (ohscrcpy_client.py)                   │
│                                                  │
│  TCP 收 NALU → OpenCV 解码 → pygame 渲染          │
│  pygame 事件 → 输入协议 → TCP 发送                │
└──────────────────────────────────────────────────┘
```

### 视频帧协议（小端序）

| 偏移 | 字段 | 类型 | 说明 |
|------|------|------|------|
| 0  | `magic`  | uint32 | 固定 `0x4F485343`（"OHSC"） |
| 4  | `flags`  | uint8  | bit0 = isKey（关键帧） |
| 5  | `pts`    | int64  | 时间戳（微秒） |
| 13 | `size`   | int32  | NALU 字节数 |
| 17 | `width`  | int32  | 视频宽度 |
| 21 | `height` | int32  | 视频高度 |
| 25 | `data`   | bytes  | H.264 NALU（Annex-B） |

新客户端接入后不会立即收到画面：服务端会先补发缓存的 SPS/PPS，再补发最近的
IDR 帧，此后转为实时推送。客户端必须按上表解析帧头。

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

### 调试：导出 H.264 码流

服务端支持 `--debug` 选项，开启后会把编码出的 H.264 码流（Annex-B 格式）写入
设备上的 `/data/capture.h264`，可直接用 ffmpeg / VLC 播放，便于排查画面异常：

```bash
hdc shell /data/local/tmp/ohscrcpy --debug
hdc file recv /data/capture.h264 ./capture.h264
ffplay capture.h264
```

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
| `capture.cpp` | 屏幕采集 + H.264 编码（Surface 模式） |
| `server.cpp` | TCP 视频服务器，发送 NALU 给客户端 |
| `input.cpp` | 输入注入服务，接收鼠标/触摸/按键事件 |
| `include/capture.h` | 屏幕采集与编码的接口定义 |
| `include/server.h` | TCP 视频服务器接口及帧协议定义 |
| `include/input.h` | 输入控制服务接口及控制协议定义 |
| `BUILD.gn` | GN 构建脚本 |
| `client/ohscrcpy_client.py` | Python 客户端（pygame + OpenCV） |

