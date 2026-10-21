#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ohscrcpy_client_opencv.py — OpenHarmony 投屏客户端（OpenCV 版）

核心改进：
  · 用 OpenCV (cv2) 替代 pygame + ffmpeg pipe，硬件解码 + 高效渲染
  · cv2.VideoCapture 直接从 pipe 读取 H.264 流，内部硬件解码
  · cv2.imshow 同一窗口显示 + cv2.setMouseCallback/waitKey 捕获输入
  · 性能：硬件解码 + 零拷贝渲染，延迟远低于 pygame 方案

依赖：
    pip install opencv-python numpy
    系统需安装 ffmpeg（OpenCV 后端）

用法：
    python ohscrcpy_client_opencv.py              # 单设备时直接运行
    python ohscrcpy_client_opencv.py -t <deviceId> # 多设备时指定
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

import cv2
import numpy as np
import pygame

# ── 协议常量 ──────────────────────────────────────────────────────────────────
FRAME_MAGIC       = 0x4F485343   # "OHSC"
HEADER_SIZE       = 25           # magic(4)+flags(1)+pts(8)+size(4)+width(4)+height(4)
NALU_FLAG_KEY     = 0x01

MSG_MOUSE_MOVE    = 0x01
MSG_MOUSE_DOWN    = 0x02
MSG_MOUSE_UP      = 0x03
MSG_MOUSE_SCROLL  = 0x04
MSG_KEY_DOWN      = 0x21
MSG_KEY_UP        = 0x22

# hdc 自动模式常量
VIDEO_SOCK_NAME = "ohscrcpy_video"
CTRL_SOCK_NAME  = "ohscrcpy_ctrl"
SERVER_BIN      = "ohscrcpy"
DEVICE_PATH     = "/data/local/tmp/ohscrcpy"
LOCAL_VIDEO_PORT = 8554
LOCAL_CTRL_PORT  = 8555

VERBOSE = False
# 窗口初始最大高度：0 = 直接用设备原始分辨率（1:1，最清晰）。
# 渲染时也不会放大超过 1:1 —— 窗口被拉大于设备分辨率时只做 letterbox，
# 避免上采样导致的发虚。
MAX_HEIGHT = 0
# 追加传给设备端 ohscrcpy 的参数（例如 "-b 20000000 -i 500"），用于调画质
SERVER_ARGS = ""
# 检测到虚拟屏（无屏设备）时，是否自动重启桌面让它在虚拟屏上重建主窗口。
# 关掉它就是旧行为：虚拟屏上只有一张壁纸，点了没有任何反应。
RESTART_LAUNCHER = True

def _log(msg):
    if VERBOSE:
        print(msg)

# ── 打包(frozen)环境下的资源定位 ─────────────────────────────────────────────
def app_dirs():
    """资源查找目录：exe 所在目录、解包临时目录(_MEIPASS)、脚本目录"""
    dirs = []
    if getattr(sys, "frozen", False):
        dirs.append(os.path.dirname(os.path.abspath(sys.executable)))
        if hasattr(sys, "_MEIPASS"):
            dirs.append(sys._MEIPASS)
    dirs.append(os.path.dirname(os.path.abspath(__file__)))
    return dirs

def find_resource(name):
    """在 app_dirs() 中查找文件，返回绝对路径或 None"""
    for d in app_dirs():
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None

def find_ffmpeg():
    """优先用打包内置的 ffmpeg.exe，否则依赖 PATH"""
    p = find_resource("ffmpeg.exe") if os.name == "nt" else find_resource("ffmpeg")
    return p or "ffmpeg"

# ── HdcSession：hdc 自动化 ───────────────────────────────────────────────────
class HdcSession:
    """单台设备的 hdc 自动化：推二进制、建端口转发、起服务端、清理。

    这里踩过三个 hdc 的坑，必须按下面的方式处理：

    1) hdc 的本地 TCP 端口是【全局】的，不按设备隔离。第二台设备再用 tcp:8554
       会直接失败（[Fail]TCP Port listen failed at 8554）；旧代码把这个错误
       吞掉后「直接复用」已有规则，于是两台设备同时看，画面永远来自第一台。
       现在按设备 ID 分配互不冲突的端口槽（视频/控制成对）。
    2) `hdc fport rm` 的任务串必须拆成【两个参数】localnode + remotenode。
       传成一个带空格的字符串只会得到 [Fail]Remove forward ruler failed,
       ruler is not exist，转发规则实际没有被删除 —— 这就是切换设备时
       「端口转发移除没生效」、新设备连上还是老画面的根因。
    3) hdc server 有时会把「已经不在 fport ls 里」的端口继续占据着
       （实测 8554/8555 规则已删，127.0.0.1:8554 仍是 LISTENING），
       这时再对同一端口 fport 必然失败。所以选端口前一定先做本地 bind 探测，
       被占用的端口直接跳过，绝不「假设已有规则可用」。
    """

    # 设备槽位数：槽 k 使用 tcp:(8554+2k) / tcp:(8555+2k)
    MAX_DEVICE_SLOTS = 40

    def __init__(self, target=None, video_port=None, ctrl_port=None):
        self.target      = target
        self.video_port  = video_port or LOCAL_VIDEO_PORT
        self.ctrl_port   = ctrl_port or LOCAL_CTRL_PORT
        # 本会话负责清理的转发规则 [(local_port, sock_name)]
        self._forwards   = []
        self._server_started = False
        self._file_pushed     = False

    def _hdc_cmd(self, *args):
        cmd = ["hdc"]
        if self.target:
            cmd += ["-t", self.target]
        cmd += list(args)
        return cmd

    @staticmethod
    def _run(cmd, check=True, timeout=10):
        cmd_str = " ".join(cmd)
        _log(f"$ {cmd_str}")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=timeout)
        except FileNotFoundError:
            raise RuntimeError("找不到 hdc 命令，请确认 hdc 已安装并在 PATH 中")
        output = (r.stdout or "").strip()
        err = (r.stderr or "").strip()
        if VERBOSE and output:
            _log(output)
        if VERBOSE and err:
            _log(err)
        if check and (r.returncode != 0 or "[Fail]" in output or "[Fail]" in err
                      or output.startswith("error")):
            raise RuntimeError(err or output or f"exit code {r.returncode}")
        return output

    # ── 端口转发 ──────────────────────────────────────────────────────────
    def _forward_rules(self):
        """解析 `hdc fport ls`，返回 {本地端口: (设备 ID, remote node)}"""
        rules = {}
        out = self._run(["hdc", "fport", "ls"], check=False, timeout=10)
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 3:
                continue
            serial, local, remote = parts[0], parts[1], parts[2]
            if not local.startswith("tcp:"):
                continue
            try:
                port = int(local[4:])
            except ValueError:
                continue
            rules[port] = (serial, remote)
        return rules

    @staticmethod
    def _port_free(port):
        """本地端口是否空闲（hdc 的 forward 监听在 127.0.0.1）"""
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False
        finally:
            s.close()

    def _add_forward(self, port, sock_name):
        out = self._run(self._hdc_cmd("fport", f"tcp:{port}",
                                      f"localabstract:{sock_name}"),
                        check=False, timeout=10)
        return ("OK" in out) or ("success" in out.lower())

    def _remove_forward(self, port, sock_name):
        """删除一条转发并确认真的删掉了。

        hdc fport rm 的正确写法是「localnode remotenode」两个独立参数：
            hdc fport rm tcp:8554 localabstract:ohscrcpy_video
        写成单个带空格的参数（旧代码的写法）会返回 ruler is not exist，
        规则仍然留在 hdc server 里，导致切设备后依然连到上一台。
        """
        out = self._run(self._hdc_cmd("fport", "rm", f"tcp:{port}",
                                      f"localabstract:{sock_name}"),
                        check=False, timeout=10)
        if "success" not in out.lower():
            # 兜底：某些 hdc 版本只按 localnode 删除
            out = self._run(self._hdc_cmd("fport", "rm", f"tcp:{port}"),
                            check=False, timeout=10) or out
        gone = port not in self._forward_rules()
        print(f"[hdc] 移除端口转发 tcp:{port}→localabstract:{sock_name}: "
              f"{'成功' if gone else '失败'} {out.strip()}".rstrip())
        return gone

    def _candidate_slots(self):
        """以设备 ID 的散列作为起点，尽量让不同设备落在不同槽位"""
        digest = zlib.crc32((self.target or "").encode("utf-8"))
        start = digest % self.MAX_DEVICE_SLOTS
        return [(start + i) % self.MAX_DEVICE_SLOTS
                for i in range(self.MAX_DEVICE_SLOTS)]

    def _setup_forwards(self):
        """为当前设备挑一组空闲端口并建立视频/控制转发"""
        rules = self._forward_rules()
        conflicts = []

        for slot in self._candidate_slots():
            vport = LOCAL_VIDEO_PORT + slot * 2
            cport = vport + 1
            wanted = ((vport, VIDEO_SOCK_NAME), (cport, CTRL_SOCK_NAME))

            # 每一步都要求「端口空闲」或「已被本设备同样的 remote 占用」
            usable = True
            for port, name in wanted:
                owner = rules.get(port)
                if owner is None:
                    if not self._port_free(port):
                        usable = False
                        conflicts.append(f"tcp:{port} 被其它程序占用")
                        break
                elif not (owner[0] == self.target
                          and owner[1].endswith(":" + name)):
                    usable = False
                    conflicts.append(
                        f"tcp:{port} 已被设备 {owner[0]} 的 {owner[1]} 占用")
                    break
            if not usable:
                continue

            created = []
            ok = True
            for port, name in wanted:
                if port in rules:
                    # 本设备上次遗留的、remote 相同的规则，直接复用
                    created.append((port, name))
                    continue
                if not self._add_forward(port, name):
                    ok = False
                    break
                created.append((port, name))

            if ok:
                self.video_port, self.ctrl_port = vport, cport
                self._forwards = created
                return
            # 回滚本槽位里刚刚新建的规则，换下一个槽
            for port, name in created:
                if port not in rules:
                    self._remove_forward(port, name)

        raise RuntimeError(
            "找不到可用的本地端口建立端口转发（视频/控制各一个）。\n  "
            + "\n  ".join(conflicts[-6:] or ["无可用槽位"])
            + "\n  可用 `hdc fport ls` 查看现有规则，"
              "或 `hdc fport rm tcp:<端口> localabstract:<名字>` 手工清理。")

    def setup(self):
        targets = self._run(["hdc", "list", "targets"])
        device_list = [l.strip() for l in targets.splitlines() if l.strip()]
        if not device_list or device_list == ["[Empty]"]:
            raise RuntimeError("没有在线设备，请先用 hdc 检查连接")

        # 无论是否显式指定 -t，都解析出具体设备 ID：
        # 端口槽位分配和转发规则归属都要靠它区分设备，否则多设备会串台。
        if self.target:
            if self.target not in device_list:
                raise RuntimeError(
                    f"设备 {self.target} 不在线\n"
                    f"  当前设备列表:\n    " + "\n    ".join(device_list or ["(空)"]))
            print(f"[hdc] 设备 {self.target} 已连接")
        else:
            if len(device_list) > 1:
                raise RuntimeError(
                    f"检测到 {len(device_list)} 个设备，请用 -t 指定设备 ID:\n    "
                    + "\n    ".join(device_list))
            self.target = device_list[0]
            print(f"[hdc] 使用默认设备: {self.target}")
        self._run(self._hdc_cmd("shell", f"killall {SERVER_BIN}"),
                  check=False, timeout=5)
        local_bin = find_resource(SERVER_BIN)
        if not local_bin:
            raise RuntimeError(
                f"找不到本地 {SERVER_BIN} 文件，请将其与程序放在同一目录")
        self._run(self._hdc_cmd("file", "send", local_bin, DEVICE_PATH), timeout=30)
        self._run(self._hdc_cmd("shell", f"chmod 755 {DEVICE_PATH}"))
        self._file_pushed = True
        print(f"[hdc] 已推送 {SERVER_BIN} → {DEVICE_PATH}")

        # 按设备分配互不冲突的本地端口，并正确清理旧规则
        self._setup_forwards()
        print(f"[hdc] 端口转发已建立: "
              f"tcp:{self.video_port}→localabstract:{VIDEO_SOCK_NAME}, "
              f"tcp:{self.ctrl_port}→localabstract:{CTRL_SOCK_NAME}")

        # setsid 让进程脱离 hdc shell 的进程组，hdc shell 退出后不会被杀
        # SERVER_ARGS 用于调画质（如 "-b 20000000 -i 500"）
        extra = f" {SERVER_ARGS.strip()}" if SERVER_ARGS and SERVER_ARGS.strip() else ""
        if extra:
            print(f"[hdc] 追加设备端编码参数:{extra}")
        cmd = self._hdc_cmd("shell",
            f"setsid {DEVICE_PATH}{extra} > /dev/null 2>&1 &")
        _log(f"$ {' '.join(cmd)}")
        try:
            subprocess.run(cmd, timeout=5,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            pass
        self._server_started = True
        print(f"[hdc] 服务端已启动（{DEVICE_PATH}）")

        # 等待服务端真正监听（最多 8 秒）
        # 通过检查 /proc/net/unix 里 abstract socket 是否出现来确认，
        # 不直接 TCP connect（否则会消耗服务端的 accept 槽位）
        deadline = time.time() + 8.0
        while time.time() < deadline:
            try:
                result = subprocess.run(
                    self._hdc_cmd("shell",
                        f"grep -c ohscrcpy_video /proc/net/unix 2>/dev/null || echo 0"),
                    capture_output=True, text=True, timeout=3
                )
                count = result.stdout.strip().split()[-1] if result.stdout.strip() else "0"
                if count.isdigit() and int(count) > 0:
                    _log("[hdc] 服务端 socket 已就绪")
                    break
            except Exception:
                pass
            time.sleep(0.4)
        else:
            print("[hdc] 警告：服务端未就绪，继续尝试...")

        self._ensure_desktop_on_virtual_screen()

    def _ensure_desktop_on_virtual_screen(self):
        """无屏设备的虚拟屏上只有一张壁纸 —— 必须让桌面重建主窗口。

        桌面 DisplayManager.createMainWindow() 特意跳过默认屏
        （注释「主屏不需要创建主窗口」），所以虚拟屏建好之后桌面不会自动在
        上面建出真正的桌面窗口，只剩 SubDisplayWallpaperPage（一张壁纸）。
        该页面不响应触摸，看起来就像「虚拟屏无法反控」——注入其实一直是通的，
        只是没有可交互的 UI。重启桌面即可让它在虚拟屏上建出正常桌面。
        """
        if not RESTART_LAUNCHER:
            return
        out = self._run(
            self._hdc_cmd("shell", "hidumper -s DisplayManagerService -a '-s -a'"),
            check=False, timeout=10)
        if "DevEcoViewer" not in out:
            return
        print("[hdc] 检测到虚拟屏（无屏设备），重启桌面让它在虚拟屏上重建主窗口")
        r = self._run(self._hdc_cmd("shell", "aa force-stop com.ohos.launcher"),
                      check=False, timeout=10)
        if "[Fail]" in r or "error" in r.lower():
            print(f"[hdc] 重启桌面失败（可手工执行 "
                  f"`hdc -t {self.target} shell \"aa force-stop com.ohos.launcher\"`）: {r}")
        else:
            time.sleep(2.0)

    def cleanup(self):
        # 先撤端口转发：它是「切设备仍看到上一台」的直接原因，
        # 放在最前面才能保证即使后面 killall / rm 卡住也已经切干净了。
        for port, sock_name in list(self._forwards):
            try:
                self._remove_forward(port, sock_name)
            except Exception as e:
                print(f"[hdc] 清理端口转发 tcp:{port} 时出错（可忽略）: {e}")
        self._forwards = []

        if self._server_started:
            try:
                self._run(self._hdc_cmd(
                    "shell", f"killall {SERVER_BIN}"
                ), check=False, timeout=5)
                print(f"[hdc] 已终止设备端 {SERVER_BIN} 进程")
            except Exception as e:
                print(f"[hdc] 终止进程时出错（可忽略）: {e}")
            self._server_started = False

        if self._file_pushed:
            try:
                self._run(self._hdc_cmd(
                    "shell", f"rm -f {DEVICE_PATH}"
                ), check=False, timeout=5)
                print(f"[hdc] 已删除设备端 {DEVICE_PATH}")
            except Exception as e:
                print(f"[hdc] 删除文件时出错（可忽略）: {e}")
            self._file_pushed = False


# ── 工具函数 ──────────────────────────────────────────────────────────────────
def recv_exact(sock, n):
    buf = bytearray(n)
    view = memoryview(buf)
    pos = 0
    while pos < n:
        r = sock.recv_into(view[pos:], n - pos)
        if r == 0:
            raise ConnectionError("连接已关闭")
        pos += r
    return bytes(buf)

def parse_nalu_header(hdr):
    magic  = struct.unpack_from("<I", hdr, 0)[0]
    flags  = hdr[4]
    pts    = struct.unpack_from("<q", hdr, 5)[0]
    size   = struct.unpack_from("<i", hdr, 13)[0]
    width  = struct.unpack_from("<i", hdr, 17)[0]
    height = struct.unpack_from("<i", hdr, 21)[0]
    return magic, flags, pts, size, width, height


# ── 控制连接 ──────────────────────────────────────────────────────────────────
class CtrlClient:
    def __init__(self, host, port):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.settimeout(5)
        self._sock.connect((host, port))
        self._sock.settimeout(None)
        self._lock = threading.Lock()
        self._closed = False
        print(f"[ctrl] 已连接 {host}:{port}")

    def _send(self, data):
        if self._closed:
            return
        with self._lock:
            if self._closed:
                return
            try:
                self._sock.sendall(data)
            except OSError as e:
                print(f"[ctrl] 发送失败: {e}，反控已断开")
                self._closed = True

    def swipe(self, x1, y1, x2, y2, duration=0.12, steps=6):
        """注入一次滑动：触摸按下 → 分步移动 → 抬起。
        等价于设备端执行 uinput -T -m x1 y1 x2 y2，用于触发屏幕刷新"""
        if self._closed:
            return
        self.mouse_button(x1, y1, 1, True)
        step = duration / steps if steps > 0 else 0
        for i in range(1, steps + 1):
            if step > 0:
                time.sleep(step)
            self.mouse_move(x1 + (x2 - x1) * i // steps,
                            y1 + (y2 - y1) * i // steps)
        time.sleep(0.03)
        self.mouse_button(x2, y2, 1, False)

    def mouse_move(self, x, y):
        self._send(struct.pack("<Bii", MSG_MOUSE_MOVE, x, y))

    def mouse_button(self, x, y, button, down):
        msg = MSG_MOUSE_DOWN if down else MSG_MOUSE_UP
        self._send(struct.pack("<BiiB", msg, x, y, button))

    def mouse_scroll(self, x, y, delta):
        self._send(struct.pack("<Biii", MSG_MOUSE_SCROLL, x, y, delta))

    def key(self, key_code, down):
        msg = MSG_KEY_DOWN if down else MSG_KEY_UP
        self._send(struct.pack("<Bi", msg, key_code))

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass


# ── NALU 转发：TCP → ffmpeg pipe → cv2.VideoCapture ──────────────────────────
class NaluRelay:
    """从 TCP 收 NALU，剥头后写入 ffmpeg stdin，供 cv2.VideoCapture 读取"""
    def __init__(self, host, port, record_path=None):
        self._host = host
        self._port = port
        self._running = False
        self._ffmpeg = None
        self._dev_width = 0
        self._dev_height = 0
        self._out_width = 0    # ffmpeg 输出分辨率（0=不缩放）
        self._out_height = 0
        self._sock = None
        self._ffmpeg_ready = threading.Event()  # ffmpeg 启动后置位
        self._out_size_ready = threading.Event()  # 主线程设好输出尺寸后置位
        self._record_path = record_path
        self._record_file = None
        self._first_data = threading.Event()   # 收到首个 NALU 后置位

    def wait_first_data(self, timeout):
        """等待首个视频 NALU 到达，返回是否已收到"""
        return self._first_data.wait(timeout)

    def set_output_size(self, w, h):
        """设置 ffmpeg 输出分辨率，通知 relay 线程可以启动 ffmpeg"""
        self._out_width = w
        self._out_height = h
        self._out_size_ready.set()

    def start(self):
        self._running = True
        threading.Thread(target=self._relay_loop, daemon=True, name="nalu-relay").start()

    def stop(self):
        self._running = False
        self._ffmpeg_ready.set()  # 解除任何等待
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        if self._ffmpeg:
            try:
                self._ffmpeg.stdin.close()
                self._ffmpeg.kill()
            except Exception:
                pass

    def get_resolution(self):
        """等待并返回设备分辨率 (width, height)"""
        for _ in range(100):
            if self._dev_width > 0 and self._dev_height > 0:
                return self._dev_width, self._dev_height
            time.sleep(0.1)
        return 1200, 1920  # 默认

    def wait_ffmpeg(self, timeout=10.0):
        """等待 ffmpeg 启动完成，返回 stdout（可读帧数据），超时返回 None"""
        self._ffmpeg_ready.wait(timeout=timeout)
        if self._ffmpeg and self._ffmpeg.stdout:
            return self._ffmpeg.stdout
        return None

    def _start_ffmpeg(self):
        """启动 ffmpeg，硬件解码 H.264，输出 bgr24 到 stdout（pipe:1）"""
        cmd = [
            find_ffmpeg(),
            "-hide_banner",
            "-loglevel", "error",
            # 低延迟输入
            "-fflags", "nobuffer",
            "-flags", "low_delay",
            "-flags2", "+fast",
            "-strict", "experimental",
            "-analyzeduration", "0",
            "-probesize", "32",
            "-hwaccel", "auto",
            "-threads", "1",
            "-f", "h264",
            "-i", "pipe:0",
            # 低延迟输出
            "-flags", "low_delay",
            "-flags2", "+fast",
            "-fflags", "nobuffer+flush_packets",
        ]
        # 若指定输出尺寸则在 ffmpeg 内缩放，省掉 Python 侧 cv2.resize
        if self._out_width > 0 and self._out_height > 0:
            cmd += ["-vf", f"scale={self._out_width}:{self._out_height}"]
        cmd += [
            "-pix_fmt", "bgr24",
            "-f", "rawvideo",
            "pipe:1",
        ]
        return subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )

    def _drain_stderr(self, ffmpeg):
        """持续读取 ffmpeg stderr，打印所有诊断信息"""
        try:
            for line in iter(ffmpeg.stderr.readline, b''):
                msg = line.decode('utf-8', 'replace').rstrip()
                if msg:
                    print(f"[ffmpeg] {msg}")
        except Exception:
            pass

    def _relay_loop(self):
        while self._running:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5)
                sock.connect((self._host, self._port))
                sock.settimeout(None)
                self._sock = sock
                print(f"[video] 已连接 {self._host}:{self._port}")

                # 读第一个 NALU 获取分辨率
                hdr = recv_exact(sock, HEADER_SIZE)
                magic, flags, pts, size, dev_w, dev_h = parse_nalu_header(hdr)
                if magic != FRAME_MAGIC:
                    print(f"[video] magic 错误: 0x{magic:08X}，重连")
                    sock.close()
                    continue
                first_nalu = recv_exact(sock, size)

                self._dev_width = dev_w
                self._dev_height = dev_h
                self._first_data.set()
                print(f"[video] 设备分辨率 {dev_w}x{dev_h}")

                # 等待主线程设好输出尺寸（win_w x win_h）
                self._out_size_ready.wait(timeout=10.0)

                # 启动 ffmpeg
                self._ffmpeg_ready.clear()
                self._ffmpeg = self._start_ffmpeg()
                out_str = f" → {self._out_width}x{self._out_height}" if self._out_width > 0 else ""
                print(f"[video] ffmpeg 已启动 pid={self._ffmpeg.pid}{out_str}")

                # 启动 stderr 读取线程（避免 stderr pipe 满导致 ffmpeg 阻塞）
                threading.Thread(
                    target=self._drain_stderr,
                    args=(self._ffmpeg,),
                    daemon=True,
                    name="ffmpeg-stderr",
                ).start()

                # 通知主线程 ffmpeg 已就绪
                self._ffmpeg_ready.set()

                # 打开录制文件（首个 NALU 是关键帧/SPS/PPS）
                if self._record_path and self._record_file is None:
                    try:
                        self._record_file = open(self._record_path, "wb")
                        print(f"[video] 录制到文件: {self._record_path}")
                    except OSError as e:
                        print(f"[video] 打开录制文件失败: {e}")

                # 写入第一个 NALU
                self._ffmpeg.stdin.write(first_nalu)
                self._ffmpeg.stdin.flush()
                if self._record_file:
                    self._record_file.write(first_nalu)

                # 持续转发 NALU
                nalu_count = 1
                while self._running:
                    try:
                        hdr = recv_exact(sock, HEADER_SIZE)
                    except socket.timeout:
                        print("[video] 5 秒未收到数据")
                        break
                    except ConnectionError as e:
                        print(f"[video] 断开: {e}")
                        break

                    magic, flags, pts, size, dev_w, dev_h = parse_nalu_header(hdr)
                    if magic != FRAME_MAGIC or size <= 0 or size > 8 * 1024 * 1024:
                        print(f"[video] NALU 异常，重连")
                        break

                    try:
                        nalu = recv_exact(sock, size)
                    except ConnectionError as e:
                        print(f"[video] 断开: {e}")
                        break

                    try:
                        self._ffmpeg.stdin.write(nalu)
                        self._ffmpeg.stdin.flush()
                    except BrokenPipeError:
                        print("[video] ffmpeg stdin 断开")
                        break

                    if self._record_file is not None:
                        try:
                            self._record_file.write(nalu)
                        except OSError as e:
                            print(f"[video] 录制写入失败: {e}")
                            self._record_file.close()
                            self._record_file = None

                    nalu_count += 1
                    is_key = bool(flags & NALU_FLAG_KEY)
                    # 前10帧全部打印，之后每30帧打印一次，帮助诊断 size 分布
                    if nalu_count <= 10 or nalu_count % 30 == 0:
                        print(f"[video] NALU #{nalu_count} size={size} key={is_key} pts={pts}")

            except OSError as e:
                if self._running:
                    print(f"[video] 连接失败: {e}，3 秒后重试...")
                    time.sleep(3)
            finally:
                if sock:
                    try:
                        sock.close()
                    except Exception:
                        pass
                if self._record_file is not None:
                    try:
                        self._record_file.close()
                    except Exception:
                        pass
                    self._record_file = None
                if self._ffmpeg:
                    try:
                        self._ffmpeg.stdin.close()
                        self._ffmpeg.kill()
                    except Exception:
                        pass


# ── OH 按键映射（cv2 key → OH KEYCODE）──────────────────────────────────────
OH_KEY_MAP = {
    13:  66,   # Enter → KEYCODE_ENTER
    8:   67,   # Backspace → KEYCODE_DEL
    27:  4,    # Esc → KEYCODE_BACK
    32:  62,   # Space → KEYCODE_SPACE
    127: 112,  # Delete → KEYCODE_FORWARD_DEL
    ord('h'): 3,  # h → KEYCODE_HOME (示例)
}


# ── 主循环 ────────────────────────────────────────────────────────────────────
def run(args):
    if args.direct:
        # 直连模式：跳过 hdc 自动化，直接连接已就绪的服务端（调试用）
        print(f"[direct] 直连 {args.direct}:{args.video_port}/{args.ctrl_port}")
        _run_app(args.direct, args.video_port, args.ctrl_port,
                 record_path=args.record)
        return

    hdc_session = HdcSession(args.target)

    try:
        hdc_session.setup()
        # 使用本次会话实际分配到的端口（多设备时每个设备端口不同）
        _run_app("127.0.0.1", hdc_session.video_port, hdc_session.ctrl_port,
                 record_path=args.record)
    finally:
        hdc_session.cleanup()


def _run_app(host, video_port, ctrl_port, record_path=None):
    """OpenCV 显示 + 输入控制主循环"""
    # 连接控制端口
    ctrl = None
    try:
        ctrl = CtrlClient(host, ctrl_port)
    except OSError as e:
        print(f"[ctrl] 连接失败: {e}，以只读模式运行")

    # 启动 NALU 转发
    relay = NaluRelay(host, video_port, record_path=record_path)
    relay.start()

    # 等待屏幕数据：OH 动态帧率下静态画面编码器不出帧会一直无数据，
    # 迟迟等不到时反控注入滑动触发出帧（实测一次可能不够，最多两次）
    for i in range(2):
        if relay.wait_first_data(timeout=2.0):
            break
        if ctrl:
            print(f"[main] 未收到视频数据，反控注入滑动触发出帧（第 {i + 1} 次）")
            ctrl.swipe(200, 200, 400, 200)
        else:
            print("[main] 未收到视频数据且反控不可用")
            break

    # 等待分辨率
    print("[main] 等待设备分辨率...")
    dev_w, dev_h = relay.get_resolution()

    # 计算窗口初始尺寸：默认按设备原始分辨率 1:1（最清晰），--max-height 可限制
    max_h = MAX_HEIGHT
    if max_h > 0 and dev_h > max_h:
        _scale = max_h / dev_h
        win_w = int(dev_w * _scale)
        win_h = max_h
    else:
        win_w, win_h = dev_w, dev_h

    # 告知 relay ffmpeg 输出尺寸（让 ffmpeg 内部缩放，省掉 Python cv2.resize）
    # 不再让 ffmpeg 缩放，改用 pygame 渲染时直接 transform.scale，减少 ffmpeg 负担
    relay.set_output_size(0, 0)  # 0 = 不缩放，输出原始 dev 分辨率

    # 等待 ffmpeg 真正启动（消除竞态）
    print("[main] 等待 ffmpeg 就绪...")
    ffmpeg_stdout = relay.wait_ffmpeg(timeout=10.0)
    if ffmpeg_stdout is None:
        print("[main] ffmpeg 启动超时，退出")
        relay.stop()
        if ctrl:
            ctrl.close()
        return

    # 创建 OpenCV 窗口
    # ── pygame 窗口 ──────────────────────────────────────────────────────────
    pygame.init()
    screen = pygame.display.set_mode((win_w, win_h), pygame.RESIZABLE)
    pygame.display.set_caption("ohscrcpy")
    clock = pygame.time.Clock()

    out_w, out_h = dev_w, dev_h  # ffmpeg 输出原始分辨率

    def _map_mouse(px, py):
        """把 pygame 窗口坐标映射回设备坐标（letterbox 感知）"""
        sw, sh = screen.get_size()
        scale = min(1.0, sw / dev_w, sh / dev_h)
        disp_w = int(dev_w * scale)
        disp_h = int(dev_h * scale)
        ox = (sw - disp_w) // 2
        oy = (sh - disp_h) // 2
        lx = px - ox
        ly = py - oy
        if disp_w > 0 and disp_h > 0:
            dx = int(lx * dev_w / disp_w)
            dy = int(ly * dev_h / disp_h)
        else:
            dx, dy = px, py
        return max(0, min(dev_w - 1, dx)), max(0, min(dev_h - 1, dy))

    print(f"[main] 设备 {dev_w}x{dev_h}，窗口 {win_w}x{win_h}")
    print("[main] 鼠标点击/拖动=触摸注入  Esc=退出  h=Home")

    frame_size = dev_w * dev_h * 3  # BGR = 3 bytes/pixel，ffmpeg 输出原始分辨率

    # ── 读帧线程：阻塞 read 放到后台，主线程只做渲染 ──────────────────────
    _stop_event = threading.Event()
    _latest_frame = [None]   # [numpy array]，只保留最新一帧
    _frame_lock   = threading.Lock()

    # 帧率统计
    _stat = {
        'decode_frames': 0,   # 解码线程累计帧数
        'render_frames': 0,   # 渲染线程累计帧数
        'dropped':       0,   # 解码快于渲染时丢弃的帧数
        'last_stat_t':   time.time(),
    }

    def _read_thread():
        buf = bytearray(frame_size)
        view = memoryview(buf)
        while not _stop_event.is_set():
            pos = 0
            try:
                while pos < frame_size:
                    n = ffmpeg_stdout.readinto(view[pos:])
                    if n == 0:
                        print("[read] ffmpeg stdout 结束")
                        _stop_event.set()
                        return
                    pos += n
            except Exception as e:
                if not _stop_event.is_set():
                    print(f"[read] 读帧异常: {e}")
                _stop_event.set()
                return

            arr = np.frombuffer(buf, dtype=np.uint8).reshape((dev_h, dev_w, 3)).copy()
            with _frame_lock:
                if _latest_frame[0] is not None:
                    _stat['dropped'] += 1   # 渲染没来得及取，直接覆盖
                _latest_frame[0] = arr
                _stat['decode_frames'] += 1

    threading.Thread(target=_read_thread, daemon=True, name="frame-read").start()

    mouse_pressed = False

    # ── 渲染主循环（pygame）────────────────────────────────────────────────────
    try:
        while not _stop_event.is_set():
            # ── 事件处理 ──
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    print("[main] 窗口已关闭")
                    _stop_event.set()
                    break
                elif event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        print("[main] 用户退出")
                        _stop_event.set()
                        break
                    elif ctrl:
                        oh_key = OH_KEY_MAP.get(event.key)
                        if oh_key:
                            _log(f"[input] 按键 {event.key} → {oh_key}")
                            ctrl.key(oh_key, True)
                            time.sleep(0.05)
                            ctrl.key(oh_key, False)
                elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    if ctrl:
                        dx, dy = _map_mouse(*event.pos)
                        mouse_pressed = True
                        _log(f"[input] 鼠标按下 → ({dx}, {dy})")
                        ctrl.mouse_button(dx, dy, 1, True)
                elif event.type == pygame.MOUSEBUTTONUP and event.button == 1:
                    if ctrl:
                        dx, dy = _map_mouse(*event.pos)
                        mouse_pressed = False
                        _log(f"[input] 鼠标抬起 → ({dx}, {dy})")
                        ctrl.mouse_button(dx, dy, 1, False)
                elif event.type == pygame.MOUSEMOTION:
                    if ctrl and mouse_pressed:
                        dx, dy = _map_mouse(*event.pos)
                        _log(f"[input] 鼠标移动 → ({dx}, {dy})")
                        ctrl.mouse_move(dx, dy)
                elif event.type == pygame.MOUSEWHEEL:
                    if ctrl:
                        mx, my = pygame.mouse.get_pos()
                        dx, dy = _map_mouse(mx, my)
                        delta = 1 if event.y > 0 else -1
                        _log(f"[input] 滚轮 delta={delta} → ({dx}, {dy})")
                        ctrl.mouse_scroll(dx, dy, delta * 120)

            if _stop_event.is_set():
                break

            # ── 取最新帧渲染 ──
            with _frame_lock:
                frame = _latest_frame[0]
                _latest_frame[0] = None

            if frame is not None:
                sw, sh = screen.get_size()
                # BGR → RGB（pygame 用 RGB）
                frame_rgb = frame[:, :, ::-1]
                surf = pygame.surfarray.make_surface(frame_rgb.swapaxes(0, 1))
                # letterbox 缩放：上限 1:1，绝不放大（放大必然发虚）
                scale = min(1.0, sw / dev_w, sh / dev_h)
                disp_w = int(dev_w * scale)
                disp_h = int(dev_h * scale)
                if disp_w != dev_w or disp_h != dev_h:
                    surf = pygame.transform.scale(surf, (disp_w, disp_h))
                ox = (sw - disp_w) // 2
                oy = (sh - disp_h) // 2
                screen.fill((0, 0, 0))
                screen.blit(surf, (ox, oy))
                pygame.display.flip()
                _stat['render_frames'] += 1
            else:
                # 没有新帧时短暂等待，避免空转
                clock.tick(120)

            # 帧率统计（每 10 秒打印一次）
            now = time.time()
            elapsed = now - _stat['last_stat_t']
            if elapsed >= 10.0:
                dec_fps = _stat['decode_frames'] / elapsed
                ren_fps = _stat['render_frames'] / elapsed
                print(f"[stat] decode={dec_fps:.1f}fps  render={ren_fps:.1f}fps"
                      f"  dropped={_stat['dropped']}  elapsed={elapsed:.1f}s")
                _stat['decode_frames'] = _stat['render_frames'] = _stat['dropped'] = 0
                _stat['last_stat_t'] = now

    except KeyboardInterrupt:
        print("\n[main] 已中断")
    finally:
        _stop_event.set()
        relay.stop()       # 关闭 socket + kill ffmpeg，解除读线程阻塞
        if ctrl:
            ctrl.close()
        pygame.quit()
        print("[main] 已退出")


def parse_args():
    p = argparse.ArgumentParser(
        description="ohscrcpy 客户端（OpenCV 版，硬件解码 + 高效渲染）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例:\n"
            "  python ohscrcpy_client_opencv.py                # 单设备直接运行\n"
            "  python ohscrcpy_client_opencv.py -t <deviceId>  # 多设备时指定\n"
        ))
    p.add_argument("-t", "--target", default=None, help="设备 ID")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="显示详细日志")
    p.add_argument("-o", "--record", default=None,
                   help="录制 H.264 流到文件（如 record.h264）")
    p.add_argument("--direct", default=None, metavar="HOST",
                   help="跳过 hdc，直接连接 HOST 的视频/控制端口（调试用）")
    p.add_argument("--video-port", type=int, default=LOCAL_VIDEO_PORT,
                   help="直连模式视频端口（默认 8554）")
    p.add_argument("--ctrl-port", type=int, default=LOCAL_CTRL_PORT,
                   help="直连模式控制端口（默认 8555）")
    p.add_argument("--max-height", type=int, default=0, metavar="N",
                   help="窗口初始最大高度像素；0（默认）= 按设备原始分辨率 1:1 显示，最清晰")
    p.add_argument("--server-args", default="", metavar="ARGS",
                   help="追加传给设备端 ohscrcpy 的参数，用于调画质，"
                        "如 --server-args \"-b 20000000 -i 500\"")
    p.add_argument("--no-restart-launcher", action="store_true",
                   help="检测到虚拟屏时也【不】重启桌面。默认会重启："
                        "否则无屏设备的虚拟屏上只有一张壁纸，点了没有任何反应")
    args = p.parse_args()
    global VERBOSE, MAX_HEIGHT, SERVER_ARGS, RESTART_LAUNCHER
    VERBOSE = args.verbose
    MAX_HEIGHT = args.max_height
    SERVER_ARGS = args.server_args or ""
    RESTART_LAUNCHER = not args.no_restart_launcher
    return args


if __name__ == "__main__":
    # OHS_LOGFILE: 同时把日志写入文件（无缓冲），便于打包版现场诊断
    _log_path = os.environ.get("OHS_LOGFILE")
    if _log_path:
        try:
            class _Tee:
                def __init__(self, *streams):
                    self._streams = streams

                def write(self, s):
                    for st in self._streams:
                        try:
                            st.write(s)
                            st.flush()
                        except OSError:
                            pass
                    return len(s)

                def flush(self):
                    for st in self._streams:
                        try:
                            st.flush()
                        except OSError:
                            pass

            _lf = open(_log_path, "w", encoding="utf-8", buffering=1)
            sys.stdout = _Tee(sys.stdout, _lf)
            sys.stderr = _Tee(sys.stderr, _lf)
        except OSError:
            pass
    try:
        run(parse_args())
    except RuntimeError as e:
        print(f"\n错误: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n已中断")
