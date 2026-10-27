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
│  PC 客户端 (ohscrcpy_client.py / .exe)            │
│                                                  │
│  TCP 收 NALU → ffmpeg 解码 → pygame 渲染          │
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
| 17 | `width`  | int32  | 视频宽度（编码帧方向） |
| 21 | `height` | int32  | 视频高度（编码帧方向） |
| 25 | `rotation` | int32 | 画面相对正立方向的旋转角（0/90/180/270） |
| 29 | `data`   | bytes  | H.264 NALU（Annex-B） |

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
pip install numpy pygame
```

另外需要系统安装 **ffmpeg** 并确保在 PATH 中（用下面的打包脚本可省掉这一步）。

## 免 Python 环境使用（打包单文件 exe）

无需在目标机器上安装 Python / numpy / pygame，也无需自己装 ffmpeg，只需
`dist/ohscrcpy_client.exe`（内嵌运行库、ffmpeg 和设备端二进制）与 hdc。

从源码重新打包（需要一台装有 Python 3.10+ 的机器）：

```powershell
pip install pyinstaller numpy pygame
powershell -ExecutionPolicy Bypass -File client\build_exe.ps1
# 产物: dist\ohscrcpy_client.exe（单文件，拷走即用）
```

`build_exe.ps1` 会自动内嵌两样外部程序，并在打包前把它们的存在性当硬校验：

| 内嵌内容 | 来源 | 说明 |
|---|---|---|
| `ohscrcpy` | `client/ohscrcpy` | 设备端可执行文件，客户端会自动 `hdc file send` 到设备 |
| `ffmpeg.exe` | PATH 中的 ffmpeg | 客户端用它解码 H.264；缺省时从 PATH 找 |

可选参数：

```powershell
# 指定要内嵌的 ffmpeg
... -File client\build_exe.ps1 -FfmpegPath C:\tools\ffmpeg.exe
# 不内嵌 ffmpeg（目标机器需自己配 PATH）
... -File client\build_exe.ps1 -NoFfmpeg
# 目录版：运行时零临时文件，但分发要带整个 dist\ohscrcpy_client\ 目录
... -File client\build_exe.ps1 -OneDir
# 保留 build 中间目录，便于排查
... -File client\build_exe.ps1 -KeepBuild
```

打包版说明：
- 单文件版启动时把内嵌内容解压到**启动时的工作目录**（双击运行、或在 exe 所在
  目录运行时即 exe 当前目录）下的 `_MEI<pid>xxxx` 子目录，**正常退出自动删除**；
  异常退出（崩溃/强杀）的残留留在原地，可放心手动删除。多设备同时开多个实例时
  各用各的 `_MEI` 目录，互不冲突。
- 因此要求启动目录可写：放在 Program Files、网络共享等只读位置会启动失败。
  完全不想要解压/临时文件时用 `-OneDir` 打目录版。
- 若启动时报 `[PYI-xxxx:ERROR] Could not create temporary directory!`，
  说明解压目录建不起来：检查启动目录是否可写，以及 exe 是否继承了
  **Low 强制完整性标签**（`icacls <exe所在目录>` 输出里出现
  `Mandatory Label\Low Mandatory Level`；修复：`icacls <目录> /setintegritylevel M`，
  注意父目录链也要查，标签会向下继承）。`build_exe.ps1` 打包后会自动检查产物
  标签并在发现问题时给出提示。
- 设备端二进制 `ohscrcpy` 已内嵌；若 exe 同目录存在同名文件则优先使用，便于替换调试。
- `OHS_LOGFILE=日志路径` 环境变量可将运行日志同时写入文件，便于现场排查。
- `--direct <host> --video-port <p> --ctrl-port <p>` 可跳过 hdc 直接连接
  已就绪的服务端端口（调试用）。
- `--no-ui-restart`（旧名 `--no-restart-launcher`）：默认在检测到虚拟屏时自动重启
  SystemUI 与桌面并解锁（否则状态栏/导航栏一直是 0x0、桌面上只剩一张壁纸，
  点了没反应），该开关用于关掉这个行为。


## 使用方法

### 无物理显示器（虚拟屏模式）

设备没接显示器时，DMS 里一块屏都没有，OH 的 `AVScreenCapture` 必然失败
（取不到默认屏返回 331350528）。而且即使补上默认屏，采集服务内部还要对默认屏做
`MakeMirror`，而 `MakeMirror` 拒绝虚拟屏作为镜像源
（`abstract_screen_controller.cpp`: `screen is nullptr, or screenType is not real.`）。

因此 ohscrcpy 现在会自动识别「没有真实屏幕」，并改用**虚拟屏直连编码器**：

1. 建一块名为 `DevEcoViewer` 的虚拟屏（该屏名在 WMS 的虚拟屏接入白名单
   `VIRTUAL_SCREEN_NAME_SET` 内，否则 WMS 会拒绝接入、该屏永远没有窗口）；
2. 把**编码器的输入 Surface** 作为该屏的渲染目标 —— RS 直接把该屏的合成结果
   渲染进编码器，完全绕开 `AVScreenCapture`；
3. 调用 `SetVirtualScreenAsDefault()` 让它成为默认屏，新建窗口才会归属到它。

有真实屏幕时行为不变（仍走 `AVScreenCapture`）。

**使用步骤：**

```bash
# 1. 启动客户端（会自动推送二进制、建转发、启动服务端；服务端自动建/复用虚拟屏）
python ohscrcpy_client.py -t <device_id>
```

客户端在检测到虚拟屏后会**自动补齐 UI**（SystemUI → 桌面 → 上滑解锁）；
不想让它动设备 UI 时加 `--no-ui-restart`（旧名 `--no-restart-launcher` 仍可用），再手工执行：

**已知限制**：
- 屏名借用系统保留的 `DevEcoViewer` 通道。
- 虚拟屏一旦被设为默认屏就**无法销毁**（`DestroyVirtualScreen` 会返回
  `DM_ERROR_INVALID_PARAM`，日志 "Not destroy default virtual screen"），
  所以 ohscrcpy 退出后它会留在 DMS 里；下次启动会自动复用
  （用 `SetVirtualScreenSurface` 把渲染目标换成新的编码器 Surface）。彻底清掉需重启设备。
- 程序退出后该屏的渲染目标（编码器 Surface）随进程消失，桌面可能停在上次画面，
  直到下次运行 ohscrcpy 重新接上 Surface。

### 画面清晰度调优

按**影响从大到小**排列：

| 手段 | 默认 | 说明 |
|---|---|---|
| **`--density <dpi>`**（服务端） | 240（VPR 1.5） | 决定 **UI 缩放比例**（VPR = dpi/160），是「看得大」与「看得细」之间的取舍，**不是单纯的清晰度开关**，详见下方说明。 |
| `-b <码率>`（服务端） | 20000000 (20 Mbps) | 码率上限，**滚动/动画等动态画面时才明显**。注意：RK 类编码器对【静态】画面按内部固定 QP 出图、无视码率（cvt 设备实测 12M/20M/40M 的静态关键帧逐字节一致），调码率不改变静态清晰度。 |
| `-i <毫秒>`（服务端） | 1000 | 关键帧间隔。**原先写死 0 → GOP=0，即不做周期性 I 帧刷新**，P 帧量化误差持续累积，动态画面会越看越糊，本次已修。 |
| `-w/-h`（服务端） | 1920×1080 | 虚拟屏分辨率。 |
| `--rate-mode <模式>`（服务端） | 不设置 | 本机硬件（`OMX.rk.video_encoder.avc`）**只支持 `cbr` / `vbr`**；`cq` / `crf` 会被硬件拒绝并导致采集**直接失败**（见下）。 |
| `--max-height N`（客户端） | 0 = 按设备原始分辨率 | 窗口初始高度。超过屏幕时自动按比例缩小到屏幕内（窗口始终带标题栏打开，可最大化/还原）。渲染侧已**禁止放大超过 1:1**，窗口被拉大时只做 letterbox，避免上采样发虚。 |
| `--server-args "..."`（客户端） | 空 | 把上面这些服务端参数从 PC 侧透传过去。 |


### 常规用法

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
| `client/ohscrcpy_client.py` | Python 客户端（ffmpeg 解码 + pygame 渲染） |
| `client/build_exe.ps1` | 打包单文件 exe 的脚本（内嵌 ffmpeg 与设备端二进制） |

