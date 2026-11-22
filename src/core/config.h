// ============================================================================
//  LumenPlayer - 应用配置
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层，只依赖自有 JSON 解析器）
//  职责    ：把"外部程序路径、引擎选择、模型路径、播放器参数"这些
//            与运行环境相关的东西集中到一个文件里，避免散落在命令行参数中。
//
//  ★ 为什么必须有配置文件（而不是全靠命令行）：
//    1. 用户不会每次都敲 "--model D:\models\x4.onnx --provider dml --runtime-dir ..."；
//    2. 播放器是双击启动的，没有命令行；
//    3. 这些参数组合起来就是"这台机器的最佳配置"，值得持久化。
//
//  ★ 相对路径规则：
//    配置里的相对路径一律相对于「配置文件所在目录」解析，而不是当前工作目录。
//    原因：双击 exe 启动时工作目录是 exe 目录，从命令行启动时是 shell 目录，
//    两种情况下 cwd 不同，用 cwd 解析会让"同一份配置"行为不一致。
// ============================================================================
#pragma once

#include <string>

#include "core/types.h"

namespace lumen {

// 超分引擎相关的配置
struct EngineConfig {
  // 后端名："dummy"（CPU 双三次，基线）| "ort"（ONNX Runtime）
  std::string backend = "dummy";
  // 执行提供程序："auto" | "cpu" | "dml" | "cuda"
  std::string provider = "auto";
  // ONNX 模型路径（ort 后端必填）
  std::string model_path;
  // onnxruntime.dll 所在目录（留空则交给系统搜索）
  std::string runtime_dir;
  // 模型输入通道数：3 = RGB，1 = 仅亮度
  int channels = 3;
  // 放大倍数：0 表示自动（按目标分辨率推断）；模型自带声明时以模型为准
  int scale = 0;
  // 分块边长：0 = 整帧；显存不足时设 256/192
  int tile_size = 0;
  bool allow_fp16 = true;
};

// 播放器（外壳）相关配置
struct PlayerConfig {
  // mpv.exe 路径。留空则依次查找：配置目录 → exe 同目录 → PATH
  std::string mpv_path;
  // 硬件解码："auto" | "no" | "d3d11va" | "dxva2"
  std::string hwdec = "auto";
  // 追加给 mpv 的额外参数（原样拼在命令行末尾，供高级用户调试）
  std::string extra_args;
  // 是否默认显示性能 HUD（F-401）。用户可用 I 键临时切换。
  bool hud = true;
  // 是否记住每个文件的播放进度（F-108）。关掉适合"反复抽查同几秒画面"的场景。
  bool resume = true;
};

struct AppConfig {
  int version = 1;

  // 外部程序。留空表示在 PATH 中查找。
  std::string ffmpeg_path;
  std::string ffprobe_path;

  // 离线导出默认输出目录（留空表示与源文件同目录）
  std::string output_dir;

  std::string preset = "balanced"; // performance | balanced | quality | archive
  bool anime = false;
  int crf = 18;

  // 场序覆盖："auto" | "tff" | "bff" | "progressive"
  // 放在顶层而不是 engine 下，因为它同时影响播放与导出两条路径。
  std::string field_order = "auto";

  EngineConfig engine;
  PlayerConfig player;

  // 序列化为带缩进的 JSON（用于生成模板，便于用户手改）
  std::string ToJson() const;
};

// 默认配置：全部指向"能跑起来的最小可用组合"
AppConfig DefaultAppConfig();

// 从文件读取配置。
// 返回值：Ok 成功；NotFound 文件不存在（调用方可用默认值继续）；
//         InvalidArgument JSON 语法错误或字段类型错误（*err 内含行内提示）
// 说明：字段缺失一律用默认值填充，只有语法错误才算失败——
//       这样用户手改配置时写漏一个字段不会导致整个程序起不来。
Status LoadAppConfig(const std::string& path, AppConfig* out, std::string* err);

// 写出一份带注释说明的默认配置（注释以 "_comment" 字段承载，JSON 本身不支持注释）
Status WriteAppConfigTemplate(const std::string& path, std::string* err);

// 相对路径解析：p 为绝对路径则原样返回；否则拼到 base_dir 后面。
// 参数说明：base_dir 需带尾部路径分隔符或为空。
std::string ResolvePath(const std::string& base_dir, const std::string& p);

// 取文件所在目录（含尾部分隔符）。用于把配置文件路径转成 base_dir。
std::string DirectoryOf(const std::string& file_path);

} // namespace lumen
