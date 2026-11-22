#!/usr/bin/env python3
# =============================================================================
#  LumenPlayer - 测试用 ONNX 模型生成器
# -----------------------------------------------------------------------------
#  用途：在没有真实超分模型（几百 MB、还要考虑许可）的情况下，验证
#        「ONNX Runtime 后端」这条链路本身是否接通：
#          张量创建 → 推理 → 输出解析 → 分块拼接 → 色彩回写
#
#  为什么用 Resize(nearest)：
#    1. 结果是确定的：输出应当严格等于输入的最近邻 2 倍放大，
#       任何偏移/串行/通道错位都会立刻暴露（真实模型做不到这种断言）；
#    2. 极小（几 KB），可以长期放在仓库里做回归；
#    3. 只用 ONNX 标准算子，opset 13，DirectML 与 CPU 都能跑。
#
#  这不是产品要用的模型。真实场景请换成 Real-ESRGAN / RealCUGAN / AnimeJaNai
#  等权重导出的 ONNX（注意 opset ≤ 19，否则 DirectML 会拒绝）。
#
#  用法：
#    python tools/gen_test_model.py testdata/models
# =============================================================================
import os
import sys


def build_nearest2x(path: str) -> None:
    import onnx
    from onnx import TensorProto, helper

    # 输入：NCHW，H/W 动态（对应"任意分辨率片源"）
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", 3, "H", "W"])
    y = helper.make_tensor_value_info("output", TensorProto.FLOAT, ["N", 3, "H2", "W2"])

    # Resize 需要 roi 与 scales 两个输入。空 roi 用空张量占位。
    roi = helper.make_tensor("roi", TensorProto.FLOAT, [0], [])
    scales = helper.make_tensor("scales", TensorProto.FLOAT, [4], [1.0, 1.0, 2.0, 2.0])

    node = helper.make_node(
        "Resize",
        inputs=["input", "roi", "scales"],
        outputs=["output"],
        mode="nearest",          # 最近邻：结果确定，便于断言
        coordinate_transformation_mode="asymmetric",
        nearest_mode="floor",
    )

    graph = helper.make_graph([node], "lumen_nearest2x", [x], [y], [roi, scales])
    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", 13)],
        producer_name="lumen-test",
    )
    model.ir_version = 8  # 兼容 ONNX Runtime 1.12+
    onnx.checker.check_model(model)
    onnx.save(model, path)


def build_conv2x(path: str, channels: int = 32, blocks: int = 4) -> None:
    """生成一个"有实际算力"的 2 倍超分网络（ESPCN 结构，权重随机初始化）。

    为什么需要它：
      nearest2x 几乎不耗算力，测出来的耗时全是数据搬运开销，用它比较
      CPU 与 DML 只会得到"GPU 更慢"的结论——那不是 GPU 的问题，是模型太轻。
      这个卷积栈才是判断"DirectML 值不值得开"的对照物：
      32 通道 4 个残差块，在 720x480 上的算力已经接近轻量超分模型。

    输出未训练（随机权重），所以画质是噪声样的——它只用于性能与链路验证，
    不能用来评画质。真要评画质请换 Real-ESRGAN 等真实权重。
    """
    import onnx
    from onnx import TensorProto, helper, numpy_helper
    import numpy as np

    inits = []
    nodes = []

    def weight(name, shape, scale=0.05):
        arr = (np.random.RandomState(hash(name) & 0xFFFF).randn(*shape) * scale).astype(
            np.float32
        )
        inits.append(numpy_helper.from_array(arr, name))

    def conv(tag, src, cin, cout, relu=True):
        """加一层 3x3 卷积（same padding），可选接 ReLU；返回输出张量名。"""
        weight(tag + "_w", [cout, cin, 3, 3])
        weight(tag + "_b", [cout], 0.0)
        out = tag + "_conv"
        nodes.append(
            helper.make_node("Conv", [src, tag + "_w", tag + "_b"], [out],
                             pads=[1, 1, 1, 1])
        )
        if not relu:
            return out
        act = tag + "_relu"
        nodes.append(helper.make_node("Relu", [out], [act]))
        return act

    cur = conv("c0", "input", 3, channels)
    # 残差块：conv → relu → conv → add
    # 有残差才能让"多层卷积"既像真实网络又不至于让激活值爆炸。
    for i in range(blocks):
        h = conv("r%da" % i, cur, channels, channels, relu=True)
        h = conv("r%db" % i, h, channels, channels, relu=False)
        out = "r%d_sum" % i
        nodes.append(helper.make_node("Add", [h, cur], [out]))
        cur = out

    # 尾段：C → 3*4 通道，再 DepthToSpace(2) 得到 2 倍分辨率
    cur = conv("cout", cur, channels, 3 * 4, relu=False)
    nodes.append(
        helper.make_node("DepthToSpace", [cur], ["output"], blocksize=2, mode="DCR")
    )

    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", 3, "H", "W"])
    y = helper.make_tensor_value_info("output", TensorProto.FLOAT, ["N", 3, "H2", "W2"])
    graph = helper.make_graph(nodes, "lumen_conv2x", [x], [y], inits)
    model = helper.make_model(
        graph,
        # opset 17：DirectML 支持范围内（>19 会被 DML 拒绝）
        opset_imports=[helper.make_opsetid("", 17)],
        producer_name="lumen-test",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, path)


def main() -> int:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "testdata/models"
    os.makedirs(out_dir, exist_ok=True)
    target = os.path.join(out_dir, "nearest2x.onnx")
    build_nearest2x(target)
    print("已生成：" + target)
    print("大小：%.1f KB" % (os.path.getsize(target) / 1024.0))
    print("用法：lumen-render in.mp4 out.mp4 --backend ort --model " + target)

    heavy = os.path.join(out_dir, "conv2x.onnx")
    build_conv2x(heavy)
    print("已生成：" + heavy)
    print("大小：%.1f KB（随机权重，仅用于性能对照）" % (os.path.getsize(heavy) / 1024.0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
