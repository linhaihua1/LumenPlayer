#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""构建单文件 LumenPlayer.exe（内嵌 mpv 运行时）。

为什么要这个脚本
----------------
之前的发布包里有 4 个 exe（player / probe / render / batch）+ 一个 mpv 目录，
用户拿到之后不知道该点哪个，也常把 mpv.exe 当成本程序去双击。
现在只产出一个 exe：mpv 运行时以 Windows 资源（RCDATA）形式嵌在里面，
首次启动时释放到 %LOCALAPPDATA%\\LumenPlayer，之后直接复用。

不用 CMake 的原因
-----------------
CMake 会把资源编译塞进构建系统，让"开发构建"和"发布构建"耦合在一起；
而这里需要的只是"开发时普通编译、发布时多一步 windres + 链接资源"。
一个 60 行的脚本比往 CMakeLists 里加一堆 if(WIN32 AND EMBED) 更好懂。

用法：
    python tools/build_single.py             # 产出 dist/LumenPlayer.exe
    python tools/build_single.py --no-embed  # 只编译（不嵌 mpv，体积小，开发用）
"""
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
EMBED_DIR = os.path.join(BUILD, "embed")
DIST = os.path.join(ROOT, "dist")
OUT_EXE = os.path.join(DIST, "LumenPlayer.exe")

SOURCES = [
    "src/apps/lumen-player/main.cpp",
    "src/apps/lumen-player/player_ui.cpp",
    "src/core/types.cpp",
    "src/core/json.cpp",
    "src/core/frame_desc.cpp",
    "src/core/frame_buffer.cpp",
    "src/core/field_analyzer.cpp",
    "src/core/color_policy.cpp",
    "src/core/grain_policy.cpp",
    "src/core/source_info.cpp",
    "src/core/processing_chain.cpp",
    "src/core/config.cpp",
    "src/platform/process.cpp",
    "src/platform/stream_pipe.cpp",
    "src/platform/mpv_ipc.cpp",
    "src/service/inspector.cpp",
]

CXXFLAGS = [
    "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", os.path.join(ROOT, "src"),
]
LDFLAGS = [
    "-static", "-static-libgcc", "-static-libstdc++",
    "-lgdi32", "-lcomdlg32", "-lshell32", "-luser32",
]


def die(msg):
    print("[失败] " + msg)
    sys.exit(1)


def find_tool(name):
    """优先 PATH，其次项目常用的便携工具链位置。"""
    found = shutil.which(name)
    if found:
        return found
    for cand in (
        os.path.join(os.path.expanduser("~"), "tools", "mingw64", "bin", name + ".exe"),
        os.path.join(ROOT, "..", "..", "tools", "mingw64", "bin", name + ".exe"),
    ):
        if os.path.exists(cand):
            return cand
    return None


def find_mpv():
    """定位一份可用的 mpv.exe（会连同同目录的 d3dcompiler_43.dll 一起嵌入）。"""
    cands = [os.path.join(ROOT, "tools", "mpv-dist", "mpv.exe")]
    cands += sorted(glob.glob(os.path.join(ROOT, "dist", "*", "mpv", "mpv.exe")),
                    reverse=True)
    cands += [
        os.path.join(ROOT, "dist", "LumenPlayer-0.2.0-win64", "mpv", "mpv.exe"),
        os.path.join(os.path.expanduser("~"), "tools", "mpv-dist", "mpv.exe"),
    ]
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def prepare_embed():
    """把 mpv 与 d3dcompiler 复制到 build/embed，并生成 .rc。"""
    mpv = find_mpv()
    if not mpv:
        die("找不到 mpv.exe。请把它放到 tools/mpv-dist/ 下，或先保留 dist 里的 mpv 目录。")

    src_dir = os.path.dirname(mpv)
    d3d = os.path.join(src_dir, "d3dcompiler_43.dll")
    if not os.path.exists(d3d):
        print("[警告] 未找到 d3dcompiler_43.dll，部分系统上 mpv 可能起不来。")

    os.makedirs(EMBED_DIR, exist_ok=True)
    shutil.copy2(mpv, os.path.join(EMBED_DIR, "mpv.exe"))
    if os.path.exists(d3d):
        shutil.copy2(d3d, os.path.join(EMBED_DIR, "d3dcompiler_43.dll"))

    # 资源 ID 必须与 main.cpp 里的 kResMpv / kResD3dCompiler 一致。
    rc = ['101 RCDATA "mpv.exe"\n']
    if os.path.exists(d3d):
        rc.append('102 RCDATA "d3dcompiler_43.dll"\n')
    rc_path = os.path.join(EMBED_DIR, "embed.rc")
    with open(rc_path, "w", encoding="utf-8") as f:
        f.writelines(rc)

    windres = find_tool("windres")
    if not windres:
        die("找不到 windres（MinGW 资源编译器）。")
    obj = os.path.join(BUILD, "embed_res.o")
    print("[1/3] 编译资源（mpv %.1f MB）…" % (os.path.getsize(mpv) / 1048576.0))
    # cwd 设为 EMBED_DIR，rc 里的相对文件名才会被解析到刚复制过来的文件上。
    r = subprocess.run([windres, "embed.rc", "-O", "coff", "-o", obj],
                       cwd=EMBED_DIR, capture_output=True, text=True)
    if r.returncode != 0:
        die("windres 失败：\n" + r.stderr)
    return obj


def main():
    embed = True
    if "--no-embed" in sys.argv:
        embed = False

    gxx = find_tool("g++")
    if not gxx:
        die("找不到 g++（MinGW-w64）。")
    print("编译器：%s" % gxx)

    os.makedirs(DIST, exist_ok=True)
    res_obj = prepare_embed() if embed else None

    print("[2/3] 编译并链接 …（内嵌资源时链接会比较慢，属正常）")
    cmd = [gxx] + CXXFLAGS + ["-o", OUT_EXE] + [os.path.join(ROOT, s) for s in SOURCES]
    if res_obj:
        cmd.append(res_obj)
    cmd += LDFLAGS

    # 链接 120MB 资源时 g++ 会先把资源读进内存再写出，给足时间。
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr)
        die("编译失败。")
    if r.stderr.strip():
        print(r.stderr.strip())

    size_mb = os.path.getsize(OUT_EXE) / 1048576.0
    print("[3/3] 完成：%s（%.1f MB）" % (OUT_EXE, size_mb))
    if embed:
        print("      单文件可直接运行，首次启动会把 mpv 释放到 "
              "%LOCALAPPDATA%\\LumenPlayer\\")


if __name__ == "__main__":
    main()
