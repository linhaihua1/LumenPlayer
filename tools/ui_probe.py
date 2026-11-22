#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LumenPlayer 界面探针 / UI smoke probe.

用途：在无人值守环境里启动播放器、按脚本发鼠标事件、截图留证。
存在的理由：mpv 用 --wid 嵌入后子窗口吞掉全部输入，很多交互只能靠
"真的点一下再截个图"来确认；这类验证必须可重复，不能靠人手点。

用法::

    # 右键弹出设置菜单并截图
    python tools/ui_probe.py --scene menu

    # 点播放按钮，确认图标变成暂停
    python tools/ui_probe.py --scene play --media D:\\clip.mkv

    # 自定义：先点 (x,y)，再截图
    python tools/ui_probe.py --click 100,200 --shot my_shot

坐标采用「客户区像素」，与窗口自身的 DPI 无关（PostMessage 不走 DPI 虚拟化）。
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import ImageGrab

user32 = ctypes.WinDLL("user32", use_last_error=True)

FindWindowW = user32.FindWindowW
FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
FindWindowW.restype = wt.HWND

GetClientRect = user32.GetClientRect
GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]

GetWindowRect = user32.GetWindowRect
GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]

ClientToScreen = user32.ClientToScreen
ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]

SetCursorPos = user32.SetCursorPos
SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]

SetForegroundWindow = user32.SetForegroundWindow
SetForegroundWindow.argtypes = [wt.HWND]

ShowWindow = user32.ShowWindow
ShowWindow.argtypes = [wt.HWND, ctypes.c_int]

SetWindowPos = user32.SetWindowPos
SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                         ctypes.c_int, ctypes.c_int, wt.UINT]

PostMessageW = user32.PostMessageW
PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

if hasattr(user32, "GetDpiForWindow"):
    GetDpiForWindow = user32.GetDpiForWindow
    GetDpiForWindow.argtypes = [wt.HWND]
    GetDpiForWindow.restype = wt.UINT
else:  # Win10 1607 之前
    GetDpiForWindow = None

SetProcessDPIAware = user32.SetProcessDPIAware

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEMOVE = 0x0200

MAIN_CLASS = "LumenPlayerMainWindow"


def dpi_of(hwnd):
    if GetDpiForWindow is not None:
        try:
            d = GetDpiForWindow(hwnd)
            if d:
                return int(d)
        except OSError:
            pass
    return 96


def px(value, dpi):
    """与 core Metrics::px 同口径：96 DPI 下的逻辑像素 → 物理像素。"""
    return int(value * dpi / 96.0 + 0.5)


def bring_to_front(hwnd):
    """把主窗口提到 Z 序顶端。

    SetForegroundWindow 在进程刚启动时经常因前台锁失败，结果截图里
    看到的是上一个应用，只有 TOPMOST 的性能浮层孤零零飘在上面。
    这里不做「最小化再恢复」——那会触发一次窗口尺寸变化，
    mpv 子窗口来不及重排就会铺满整个客户区，把侧栏和控制条盖掉。
    """
    ShowWindow(hwnd, 9)   # SW_RESTORE（未最小化时是空操作）
    SetForegroundWindow(hwnd)
    # 探针期间临时置顶：连续多次启动/结束时前台锁会漂，主窗口一旦被别的
    # 应用盖住，菜单模态循环就收不到注入的按键。进程结束后自动失效。
    SetWindowPos(hwnd, wt.HWND(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)  # HWND_TOPMOST
    time.sleep(0.5)


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def wait_window(timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = FindWindowW(MAIN_CLASS, None)
        if h:
            return h
        time.sleep(0.2)
    return None


def click(hwnd, x, y, right=False, settle=0.12):
    """先移动真实光标（菜单按光标定位），再 PostMessage 一次完整点击。"""
    pt = wt.POINT(x, y)
    ClientToScreen(hwnd, ctypes.byref(pt))
    SetCursorPos(pt.x, pt.y)
    time.sleep(settle)
    down, up = (WM_RBUTTONDOWN, WM_RBUTTONUP) if right else (WM_LBUTTONDOWN, WM_LBUTTONUP)
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam(x, y))
    time.sleep(0.05)
    PostMessageW(hwnd, down, 2 if right else 1, lparam(x, y))
    time.sleep(0.05)
    PostMessageW(hwnd, up, 0, lparam(x, y))


keybd_event = user32.keybd_event
keybd_event.argtypes = [ctypes.c_ubyte, ctypes.c_ubyte, wt.DWORD, ctypes.c_size_t]


mouse_event = user32.mouse_event
mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]


def real_click(hwnd, x, y, settle=1.0):
    """注入真实鼠标点击到光标所在窗口（菜单是独立窗口，PostMessage 打不到它）。"""
    pt = wt.POINT(x, y)
    ClientToScreen(hwnd, ctypes.byref(pt))
    SetCursorPos(pt.x, pt.y)
    time.sleep(0.25)
    mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    time.sleep(0.06)
    mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    time.sleep(settle)


def press_keys(codes, gap=0.25):
    """产生真实键盘输入。

    菜单弹出后 TrackPopupMenu 会进入自己的模态消息循环，PostMessage 到主窗口
    的按键根本不会到达它；只有注入到硬件输入队列的按键才一定被菜单处理。
    """
    for vk in codes:
        keybd_event(vk, 0, 0, 0)
        time.sleep(0.03)
        keybd_event(vk, 0, 0x0002, 0)  # KEYEVENTF_KEYUP
        time.sleep(gap)


def hover(hwnd, x, y, settle=1.2):
    """把真实光标移到客户区某点并等待（用于让带子菜单的项自动展开）。"""
    pt = wt.POINT(x, y)
    ClientToScreen(hwnd, ctypes.byref(pt))
    SetCursorPos(pt.x, pt.y)
    time.sleep(settle)
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam(x, y))


def screenshot(hwnd, path):
    """只截客户区。

    截窗口矩形会把 8px 的隐形调整边框也算进来，边上会露出底下的桌面，
    看起来像"界面渲染坏了"。换算成客户区原点 + 客户区尺寸才是应用真正画的那块。
    """
    rc = wt.RECT()
    GetClientRect(hwnd, ctypes.byref(rc))
    origin = wt.POINT(0, 0)
    ClientToScreen(hwnd, ctypes.byref(origin))
    box = (origin.x, origin.y, origin.x + rc.right, origin.y + rc.bottom)
    img = ImageGrab.grab(bbox=box, all_screens=True)
    img.save(path)
    return img.size


def dump_children(hwnd, tag):
    """打印子窗口（mpv vo 窗口）的客户区坐标，定位布局覆盖问题。"""
    EnumChildWindows = user32.EnumChildWindows
    EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]
    GetClassNameW = user32.GetClassNameW
    GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(child, _):
        name = ctypes.create_unicode_buffer(128)
        GetClassNameW(child, name, 128)
        rc = wt.RECT()
        GetWindowRect(child, ctypes.byref(rc))
        origin = wt.POINT(0, 0)
        ClientToScreen(hwnd, ctypes.byref(origin))
        out.append("%s@(%d,%d %dx%d)" %
                   (name.value, rc.left - origin.x, rc.top - origin.y,
                    rc.right - rc.left, rc.bottom - rc.top))
        return True

    EnumChildWindows(hwnd, cb, 0)
    print("[%s] children: %s" % (tag, " ".join(out) if out else "(none)"))


def geometry(hwnd):
    """读当前客户区尺寸与 DPI，并按 player_ui.cpp 的公式算出各按钮位置。

    必须"用之前"现读：窗口启动时先按内容定尺寸，之后还可能被 maximize
    或用户配置改成别的样子，一开始读到的值不一定是最终值。
    """
    rc = wt.RECT()
    GetClientRect(hwnd, ctypes.byref(rc))
    cw, ch = rc.right, rc.bottom
    dpi = dpi_of(hwnd)

    controls_h = px(88, dpi)
    sidebar_w = min(px(304, dpi), cw // 3)
    if sidebar_w < px(230, dpi):
        sidebar_w = 0
    controls_w = cw - sidebar_w
    row_y = ch - controls_h + px(34, dpi)
    row_h = px(38, dpi)
    mid = row_y + row_h // 2

    return {
        "cw": cw,
        "ch": ch,
        "dpi": dpi,
        # 右侧自右向左：全屏(36) → 设置(36)；从右边缘到设置左边缘 = 14+2+36+36
        "gear": (controls_w - px(88, dpi) + px(18, dpi), mid),
        "fullscreen": (controls_w - px(14, dpi) - px(18, dpi), mid),
        # 左侧自左向右：打开(34) → 后退(40) → 播放(46) → 前进(40)
        "open": (px(14, dpi) + px(17, dpi), mid),
        "back": (px(14, dpi) + px(34, dpi) + px(4, dpi) + px(20, dpi), mid),
        "play": (px(14, dpi) + px(34, dpi) + px(4, dpi) + px(40, dpi) + px(2, dpi) + px(23, dpi), mid),
        "video_mid": ((cw - sidebar_w) // 2, (ch - controls_h) // 2),
    }



def main():
    ap = argparse.ArgumentParser(description="LumenPlayer UI probe")
    ap.add_argument("--exe", default=r"dist\LumenPlayer.exe")
    ap.add_argument("--media", default=None, help="启动时直接播放的文件")
    ap.add_argument("--scene", default=None,
                    choices=["menu", "gear", "play", "sidebar", "fs_cycle", "resize_cycle"])
    ap.add_argument("--fs-repeat", type=int, default=1, help="全屏切换次数（fs_cycle 用）")
    ap.add_argument("--click", default=None, help="客户区坐标 x,y（可重复，分号分隔多个）")
    ap.add_argument("--rclick", default=None, help="右键坐标 x,y")
    ap.add_argument("--hover", default=None, help="菜单弹出后悬停的客户区坐标 x,y")
    ap.add_argument("--keys", default=None, help="菜单弹出后依次按下的虚拟键码，逗号分隔")
    ap.add_argument("--realclick", default=None, help="菜单弹出后真实鼠标点击的客户区坐标 x,y")
    ap.add_argument("--shot", default=None, help="截图文件名（不含扩展名）")
    ap.add_argument("--outdir", default=r"build\shots")
    ap.add_argument("--settle", type=float, default=2.0, help="操作后等待秒数（菜单动画/重绘）")
    ap.add_argument("--keep", action="store_true", help="截完不杀进程")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        sys.exit("找不到可执行文件: %s" % args.exe)

    SetProcessDPIAware()
    outdir = os.path.abspath(args.outdir)
    os.makedirs(outdir, exist_ok=True)

    cmd = [os.path.abspath(args.exe)]
    if args.media:
        cmd.append(os.path.abspath(args.media))
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(os.path.abspath(args.exe)))

    hwnd = wait_window()
    if not hwnd:
        proc.kill()
        sys.exit("窗口没起来（LumenPlayerMainWindow 未找到）")

    time.sleep(3.0)  # mpv 起来 + 首帧 + 首次信息刷新
    bring_to_front(hwnd)
    time.sleep(0.6)

    g = geometry(hwnd)
    print("client=%dx%d dpi=%d" % (g["cw"], g["ch"], g["dpi"]))
    dump_children(hwnd, "startup")

    shots = []
    if args.scene == "menu":
        click(hwnd, g["video_mid"][0], g["video_mid"][1], right=True)
        time.sleep(args.settle)
        shots.append("menu_rightclick")
    elif args.scene == "gear":
        click(hwnd, g["gear"][0], g["gear"][1])
        time.sleep(args.settle)
        shots.append("menu_gear")
    elif args.scene == "play":
        click(hwnd, g["play"][0], g["play"][1])
        time.sleep(args.settle)
        shots.append("play_toggled")
    elif args.scene == "sidebar":
        time.sleep(args.settle)
        shots.append("sidebar")
    elif args.scene == "fs_cycle":
        # 进入全屏 -> 截图 -> 退出全屏 -> 截图。定位"退出后控制条消失"的回归点。
        WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
        for i in range(args.fs_repeat):
            PostMessageW(hwnd, WM_KEYDOWN, ord("F"), 0)
            PostMessageW(hwnd, WM_KEYUP, ord("F"), 0)
            time.sleep(0.7)
            dump_children(hwnd, "fs#%d entered" % (i + 1))
            if i == 0:
                path = os.path.join(outdir, "fs_entered.png")
                screenshot(hwnd, path)
                print("saved %s" % path)
            PostMessageW(hwnd, WM_KEYDOWN, ord("F"), 0)
            PostMessageW(hwnd, WM_KEYUP, ord("F"), 0)
            time.sleep(0.7)
            dump_children(hwnd, "fs#%d exited" % (i + 1))
        bring_to_front(hwnd)
        dump_children(hwnd, "exited+front")
        shots.append("fs_exited")
    elif args.scene == "resize_cycle":
        # 最大化 -> 还原：与全屏切换同样是父窗口尺寸变化，验证布局夺回通用有效。
        dump_children(hwnd, "normal")
        ShowWindow(hwnd, 3)   # SW_MAXIMIZE
        time.sleep(0.7)
        dump_children(hwnd, "maximized")
        ShowWindow(hwnd, 9)   # SW_RESTORE
        time.sleep(0.7)
        dump_children(hwnd, "restored")
        bring_to_front(hwnd)
        shots.append("resize_restored")

    for spec in (args.click or "").split(";"):
        spec = spec.strip()
        if not spec:
            continue
        x, y = (int(v) for v in spec.split(","))
        click(hwnd, x, y)
        time.sleep(0.6)

    if args.rclick:
        x, y = (int(v) for v in args.rclick.split(","))
        click(hwnd, x, y, right=True)
        time.sleep(args.settle)

    if args.hover:
        x, y = (int(v) for v in args.hover.split(","))
        hover(hwnd, x, y)

    if args.keys:
        press_keys([int(v, 0) for v in args.keys.split(",")])
        time.sleep(0.5)

    if args.realclick:
        x, y = (int(v) for v in args.realclick.split(","))
        real_click(hwnd, x, y)

    if args.shot and args.shot not in shots:
        shots.append(args.shot)

    if not shots:
        shots.append("shot")

    for name in shots:
        path = os.path.join(outdir, name + ".png")
        # 不在这里再次置前：菜单弹出时改前台窗口会把 TrackPopupMenu 关掉
        w, h = screenshot(hwnd, path)
        print("saved %s  (%dx%d)" % (path, w, h))

    if not args.keep:
        time.sleep(0.3)
        proc.kill()
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
