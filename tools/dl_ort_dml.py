# -*- coding: utf-8 -*-
"""下载 ONNX Runtime DirectML 运行时（nupkg 里含 onnxruntime.dll + DirectML.dll）。

用途：验证 DirectML 执行提供程序（GPU 推理）在真机上是否可用。
nupkg 本质是 zip，取 runtimes/win-x64/native/ 下的两个 DLL 即可。

用法：python tools/dl_ort_dml.py [版本号]
"""
import io
import os
import sys
import zipfile
import urllib.request

VER = sys.argv[1] if len(sys.argv) > 1 else "1.20.1"
PKG = f"microsoft.ml.onnxruntime.directml.{VER}"
URLS = [
    f"https://nuget.cdn.azure.cn/v3-flatcontainer/microsoft.ml.onnxruntime.directml/{VER}/microsoft.ml.onnxruntime.directml.{VER}.nupkg",
    f"https://globalcdn.nuget.org/packages/microsoft.ml.onnxruntime.directml.{VER}.nupkg",
    f"https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/{VER}",
]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "ort-dml")
WANT = ("onnxruntime.dll", "DirectML.dll")


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "lumen-dl/1.0"})
    return urllib.request.urlopen(req, timeout=timeout).read()


def main():
    os.makedirs(OUT, exist_ok=True)
    data = None
    for u in URLS:
        try:
            print(f"[尝试] {u}")
            data = fetch(u)
            print(f"[成功] {len(data)/1048576:.1f} MB")
            break
        except Exception as e:  # noqa: BLE001
            print(f"[失败] {e}")
    if data is None:
        print("全部镜像失败")
        return 1

    # 只取 win-x64：nupkg 里同时带 x86 / arm64 的同名 DLL，
    # 一开始按 basename 平铺解压导致 arm64 版本覆盖了 x64（三者同名），
    # 表现为"加载成功但推理报错"。按架构建子目录彻底避免这种覆盖。
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        picked = 0
        for n in z.namelist():
            if not n.lower().startswith("runtimes/win-x64/native/"):
                continue
            base = os.path.basename(n)
            if not base.lower().endswith((".dll",)):
                continue
            dest = os.path.join(OUT, "win-x64", base)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(n) as src, open(dest, "wb") as dst:
                dst.write(src.read())
            print(f"[解出] win-x64/{base}  {os.path.getsize(dest)/1048576:.1f} MB")
            picked += 1
        if picked == 0:
            print("包内未找到 win-x64/native/*.dll")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
