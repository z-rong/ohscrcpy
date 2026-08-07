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

def _log(msg):
    if VERBOSE:
        print(msg)

# ── HdcSession：hdc 自动化 ───────────────────────────────────────────────────
class HdcSession:
    def __init__(self, target=None, video_port=LOCAL_VIDEO_PORT, ctrl_port=LOCAL_CTRL_PORT):
        self.target      = target
        self.video_port  = video_port
        self.ctrl_port   = ctrl_port
        self._fport_set  = False
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
            output = (r.stdout or "").strip()
            if VERBOSE and output:
                _log(output)
            if VERBOSE and (r.stderr or "").strip():
                _log((r.stderr or "").strip())
            if check:
                if r.returncode != 0 or "[Fail]" in output or output.startswith("error"):
                    msg = (r.stderr or "").strip() or output or f"exit code {r.returncode}"
                    raise RuntimeError(f"{msg}")
            return output
        except FileNotFoundError:
            raise RuntimeError(f"找不到 hdc 命令，请确认 hdc 已安装并在 PATH 中")

    def setup(self):
        targets = self._run(["hdc", "list", "targets"])
        device_list = [l.strip() for l in targets.splitlines() if l.strip()]
        if not device_list or device_list == ["[Empty]"]:
            raise RuntimeError("没有在线设备，请先用 hdc 检查连接")

        if self.target:
            if self.target not in targets:
                raise RuntimeError(
                    f"设备 {self.target} 不在线\n"
                    f"  当前设备列表:\n    " + "\n    ".join(device_list or ["(空)"]))
            print(f"[hdc] 设备 {self.target} 已连接")
        else:
            if len(device_list) > 1:
                raise RuntimeError(
                    f"检测到 {len(device_list)} 个设备，请用 -t 指定设备 ID:\n    "
                    + "\n    ".join(device_list))
            print(f"[hdc] 使用默认设备: {device_list[0]}")

        self._run(self._hdc_cmd("shell", f"killall {SERVER_BIN}"),
                  check=False, timeout=5)
        local_bin = os.path.join(os.path.dirname(os.path.abspath(__file__)), SERVER_BIN)
        if not os.path.isfile(local_bin):
            raise RuntimeError(f"找不到本地 {SERVER_BIN} 文件: {local_bin}")
        self._run(self._hdc_cmd("file", "send", local_bin, DEVICE_PATH), timeout=30)
        self._run(self._hdc_cmd("shell", f"chmod 755 {DEVICE_PATH}"))
        self._file_pushed = True
        print(f"[hdc] 已推送 {SERVER_BIN} → {DEVICE_PATH}")

        for port, sock_name in [(self.video_port, VIDEO_SOCK_NAME),
                                 (self.ctrl_port, CTRL_SOCK_NAME)]:
            self._run(self._hdc_cmd(
                "fport", "rm", f"tcp:{port} localabstract:{sock_name}"
            ), check=False, timeout=5)
            try:
                self._run(self._hdc_cmd(
                    "fport", f"tcp:{port}", f"localabstract:{sock_name}"
                ))
            except RuntimeError as e:
                err = str(e)
                if "listen failed" in err or "Port listen" in err:
                    _log(f"端口 tcp:{port} 已有转发规则，直接复用")
                else:
                    raise
        self._fport_set = True
        print(f"[hdc] 端口转发已建立: "
              f"tcp:{self.video_port}→localabstract:{VIDEO_SOCK_NAME}, "
              f"tcp:{self.ctrl_port}→localabstract:{CTRL_SOCK_NAME}")

        # setsid 让进程脱离 hdc shell 的进程组，hdc shell 退出后不会被杀
        cmd = self._hdc_cmd("shell",
            f"setsid {DEVICE_PATH} --abstract > /dev/null 2>&1 &")
        _log(f"$ {' '.join(cmd)}")
        try:
            subprocess.run(cmd, timeout=5,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            pass
        self._server_started = True
        print(f"[hdc] 服务端已启动（{DEVICE_PATH} --abstract）")

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

    def cleanup(self):
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

        if self._fport_set:
            try:
                self._run(self._hdc_cmd(
                    "fport", "rm",
                    f"tcp:{self.video_port} localabstract:{VIDEO_SOCK_NAME}"
                ), check=False, timeout=5)
                self._run(self._hdc_cmd(
                    "fport", "rm",
                    f"tcp:{self.ctrl_port} localabstract:{CTRL_SOCK_NAME}"
                ), check=False, timeout=5)
                print("[hdc] 端口转发已移除")
            except Exception as e:
                print(f"[hdc] 清理端口转发时出错（可忽略）: {e}")
            self._fport_set = False


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
            "ffmpeg",
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
    hdc_session = HdcSession(args.target)
    hdc_session.setup()

    try:
        _run_app("127.0.0.1", LOCAL_VIDEO_PORT, LOCAL_CTRL_PORT,
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

    # 等待分辨率
    print("[main] 等待设备分辨率...")
    dev_w, dev_h = relay.get_resolution()

    # 计算窗口初始尺寸（限制最大高度960）
    max_h = 960
    if dev_h > max_h:
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
        scale = min(sw / dev_w, sh / dev_h)
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
                # letterbox 缩放
                scale = min(sw / dev_w, sh / dev_h)
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
    args = p.parse_args()
    global VERBOSE
    VERBOSE = args.verbose
    return args


if __name__ == "__main__":
    try:
        run(parse_args())
    except RuntimeError as e:
        print(f"\n错误: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n已中断")
