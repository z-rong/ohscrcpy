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

## 免 Python 环境使用（打包单文件 exe）

无需在目标机器上安装 Python / OpenCV / ffmpeg，只需 `dist/ohscrcpy_client.exe`
（约 104MB，已内嵌运行库、ffmpeg 和设备端二进制）与 hdc。

从源码重新打包（需要一台装有 Python 3.10+ 的机器）：

```powershell
pip install pyinstaller opencv-python pygame numpy
powershell -ExecutionPolicy Bypass -File client\build_exe.ps1
# 产物: dist\ohscrcpy_client.exe
```

打包版说明：
- 首次启动时 exe 会把内嵌的运行库释放到系统临时目录（`%TEMP%\_MEIxxxx`），
  属 PyInstaller 单文件机制的正常行为，退出后自动清理；启动比脚本版慢数秒。
- 设备端二进制 `ohscrcpy` 已内嵌；若 exe 同目录存在同名文件则优先使用，便于替换调试。
- `OHS_LOGFILE=日志路径` 环境变量可将运行日志同时写入文件，便于现场排查。
- `--direct <host> --video-port <p> --ctrl-port <p>` 可跳过 hdc 直接连接
  已就绪的服务端端口（调试用）；配合 `client/mock_device.py` 可在无真机环境下
  做端到端测试。
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

```bash
hdc -t <device_id> shell "killall com.ohos.systemui"              # 状态栏/导航栏 0x0
hdc -t <device_id> shell "aa force-stop com.ohos.launcher"        # 桌面只建了壁纸窗口
# 然后手工在画面上从底部上滑一次解锁
```

**为什么必须补这一步：**

- **SystemUI（状态栏 / 导航栏 / 锁屏）**：SystemUI 在开机时初始化
  `NavBarConfiguration.initNavBarConfiguration()`：
  ```ts
  display.getDefaultDisplay().then(dis => { maxWidth = dis.width; ... })
  ```
  此刻无屏设备上系统里一块屏都没有 → 该 Promise 失败 → `maxWidth/maxHeight`
  保持 `undefined` → 窗口按 **0×0** 创建。而 SystemUI 只注册了
  `display.on('change')`（且仅当「方向变化」时才 `resetSizeWindow`），
  虚拟屏是 `display.on('add')` 事件，**永远不会触发重排** —— 于是
  `SystemUi_StatusBar / NavigationBar / PrivacyIndicator / ScreenLockWindow`
  一直是 `[0,0,0,0]`，看起来就是「SystemUI 没起来」。
  重启 SystemUI，它会按真实分辨率重建窗口（1920×75 等）。
- **桌面**：`DisplayManager.createMainWindow()` 特意跳过默认屏
  （"主屏不需要创建主窗口"），所以光把虚拟屏设为默认屏，桌面不会在上面建窗口；
  它只在启动时按 `display.getDefaultDisplaySync().id` 判断一次「谁是主屏」，
  之后不再复查。重启桌面即可让它在虚拟屏上建出正常桌面。
- **解锁**：SystemUI 起来后 keyguard 才真正生效（`ScreenLockWindow` 变成全屏），
  会盖住桌面，需要一次从底部上滑解锁。

> 注意顺序：服务端的 abstract socket **先于**虚拟屏创建，客户端必须**等虚拟屏
> 真正出现**（轮询 `hidumper -s DisplayManagerService -a '-s -a'` 里的
> `DevEcoViewer` + 默认屏）再去重启 SystemUI，否则 SystemUI 又会在「没有屏」的
> 状态下重启一次，窗口依旧是 0×0。

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
| `-b <码率>`（服务端） | 12000000 (12 Mbps) | 码率上限，**滚动/动画等动态画面时才明显**。注意：**原先 `-b` 根本没接进 getopt**（帮助里写了但解析不到），实际一直固定 4 Mbps，本次已修。 |
| `-i <毫秒>`（服务端） | 1000 | 关键帧间隔。**原先写死 0 → GOP=0，即不做周期性 I 帧刷新**，P 帧量化误差持续累积，动态画面会越看越糊，本次已修。 |
| `-w/-h`（服务端） | 1920×1080 | 虚拟屏分辨率。 |
| `--rate-mode <模式>`（服务端） | 不设置 | 本机硬件（`OMX.rk.video_encoder.avc`）**只支持 `cbr` / `vbr`**；`cq` / `crf` 会被硬件拒绝并导致采集**直接失败**（见下）。 |
| `--max-height N`（客户端） | 0 = 按设备原始分辨率 | 窗口初始高度。渲染侧已**禁止放大超过 1:1**，窗口被拉大时只做 letterbox，避免上采样发虚。 |
| `--server-args "..."`（客户端） | 空 | 把上面这些服务端参数从 PC 侧透传过去。 |

#### 关于 `--density`：这是缩放取舍，不是清晰度开关

同一个 UI，在两种密度下的表现不同，取决于你怎么看：

| 比较方式 | VPR 1.5（默认 240dpi） | VPR 1.0（160dpi） |
|---|---|---|
| **按 1:1 显示**（客户端默认，你实际看到的） | 元素更大更好读，但每个元素被放大 1.5 倍，观感偏柔 | 元素更小，但逐像素映射，观感更"利" |
| **归一化到同一元素尺寸**（比信息量） | 图标细节**更多**（更高密度会取用更高分辨率的图片资源） | 图标被缩小显示，细节反而更少 |

所以：**想读得大就保持 240；想逐像素锐利、且能接受 UI 变小就用 160**；
两者之间可以试 `--density 200`（VPR 1.25）折中。

> ⚠️ 修正说明：本文档早期版本曾把「改 `--density 160`」写成主要的清晰度提升手段。
> 那个结论来自**同一放大倍数**下的目视对比，且两张样本**一张是串流帧、一张是原始快照**，
> 不是同类对比。上面这张表是重新做了尺寸归一化对比后的结论。

推荐（先保持平台默认密度，只把码率/关键帧和客户端 1:1 打开）：

```bash
python ohscrcpy_client.py -t <device_id> --server-args "-b 20000000 -i 500"
```

若想 UI 更大或更小，再追加 `--density 160`（更小更利）或 `--density 200`（折中）。

#### 排查过程（怎么定位到「不是编码的锅」）

1. 放大画面看，字形边缘是柔和渐变 → 第一反应是编码码率不够；
2. 先修 `-b`（发现它根本没生效）并把默认提到 12 Mbps，再把 `-i` 从 0 改成 1000；
3. 但用 Laplacian 方差量化对比，改前 6772 / 改后 6769 —— **几乎没变化**；
4. 换用设备自带 `snapshot_display` 抓**完全没经过 H.264 的原始画面**，同区域得分反而更低（6155）
   → **证明编码不是瓶颈**；改动 `-b` 1/4/12/24 Mbps 时编码器实际输出恒为 0.73~0.80 Mbps，
   也说明静态画面下码率上限根本不是约束；
5. 最终确认：**1080p 下这条链路（虚拟屏→RS→编码器→客户端）已经是透明的**，
   没有明显可再榨取的空间；真正决定观感的是平台密度与客户端显示缩放。

> 顺带：`snapshot_display` 现在可用（虚拟屏成了默认屏之后），是个很好用的画质排查工具：
> `hdc shell "snapshot_display -f /data/local/tmp/s.jpeg"` + `hdc file recv`（只支持 .jpeg）。

#### 虚拟屏与真实屏的对比（跨设备实测）

在带屏设备 `swqaewqe` 上同样跑一遍，得到两条关键结论：

**1. 虚拟屏不引入任何画质差异 —— 与真实屏逐像素一致**

`swqaewqe` 的真实屏是 `Unknown REAL, 1920x1080, VPR 1.5`；无屏设备的虚拟屏
设为同样参数后，两边用 `snapshot_display` 取原始画面，**4 个采样区域 MD5 完全相同**：

```
760:170:0:140        完全一致 ✔
600:200:600:300      完全一致 ✔
500:120:1200:700     完全一致 ✔
300:100:1600:150     完全一致 ✔
```

**2. 所以「模糊」在带屏设备上同样存在**

`swqaewqe` 的真实屏**也是 VPR 1.5** —— 这个密度是平台默认值，不是虚拟屏特有的。
两边渲染逐像素一致，也就意味着**同样的柔和程度**。串流侧同样持平：
同区域 Laplacian 方差 无屏虚拟屏 **10089** vs 带屏真实屏 **9841**。

**3. 代码路径无回归**

带屏设备上 `HasRealDisplay()` 正确识别（日志 `发现真实屏幕: Unknown (id=0)`），
走原来的 `AVScreenCapture`，**不会创建虚拟屏**：

```
I ohscrcpy_vdisplay: 发现真实屏幕: Unknown (id=0)
I ohscrcpy: 真实屏幕分辨率: 1920x1080
I ohscrcpy_capture: 屏幕采集+编码已启动 1920x1080 @30fps
```

**4. 排除的两个嫌疑**

- `VirtualScreenOption::isForShot_`：只是 IPC 透传字段（默认就是 `true`），不参与渲染决策；
- `VirtualScreenFlag`：取值只有 `DEFAULT=0 / CAST=1 / MAX=2`，我们用 `DEFAULT` 是对的。

因此**在 1080p 不变的前提下，能动的只有密度**（`--density`）：
这是「UI 更大但被放大 1.5 倍」与「1:1 最清晰但 UI 更小」之间的取舍。

#### 硬件码率模式实测

```
--rate-mode cbr → HCODEC: set 0 mode and target bitrate 12000000 bps succ    ✅ 110 NALUs
--rate-mode vbr → HCODEC: set 1 mode and target bitrate 12000000 bps succ    ✅ 110 NALUs
--rate-mode cq  → HCODEC: SetConstantQualityMode failed
                  (OMX_IndexParamControlRateConstantQuality)                 ❌ 0 NALUs，采集失败
--rate-mode crf → HCODEC: SetCRFMode failed (OMX_IndexParamControlRateCRF)   ❌ Configure 直接失败
```

即这台板子的编码器**没有恒定质量（CQ/CRF）能力**，所以「按质量分配码率」这条路走不通，
清晰度只能从**渲染分辨率/密度**和**码率上限**这两处调。

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

### 多设备同时连接 / 切换设备

本地端口**不是**按设备隔离的：`hdc` 的 `fport` 在**整台 PC 上全局绑定**
`127.0.0.1:<port>`。所以两台设备都用 `tcp:8554` 时，第二台必然失败：

```
[Fail]TCP Port listen failed at 8554
```

因此客户端现在**按设备 ID 分配端口槽**（视频/控制成对），并把实际使用的端口打印出来：

```
[hdc] 端口转发已建立: tcp:8590→localabstract:ohscrcpy_video, tcp:8591→localabstract:ohscrcpy_ctrl
```

两台设备可以同时开两个客户端，互不影响。

另外两个必须注意的 `hdc` 行为（客户端已处理，手工操作时也要小心）：

1. **`hdc fport rm` 的任务串必须是两个参数**：
   ```bash
   hdc fport rm tcp:8554 localabstract:ohscrcpy_video     # 正确
   hdc fport rm "tcp:8554 localabstract:ohscrcpy_video"   # 错误：ruler is not exist，规则没删掉
   ```
   后者会返回 `[Fail]Remove forward ruler failed, ruler is not exist`，
   但规则其实还在 —— 于是「切换设备后连的仍是上一台」。
   客户端现在会删完再 `fport ls` 复核，确认端口真的释放。
2. `hdc fport ls` 里没有、但 `127.0.0.1:<port>` 仍处于 `LISTENING` 的情况确实存在
   （hdc server 残留监听）。这时再 `fport` 同一个端口一定失败，
   所以客户端选端口前会先做本地 bind 探测，被占用就换下一个槽，
   绝不再「假设已有规则可复用」。彻底清掉残留只能重启 hdc server（`hdc kill`）。

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

