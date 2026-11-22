# 第三方组件许可说明

本项目自有代码采用 [LICENSE](./LICENSE)（LNC-SL 1.0：源代码公开，商用需授权）。
下列第三方组件**不受本协议约束**，按各自原许可使用；本项目的商业授权不涉及、
也不能豁免这些组件的许可义务。

| 组件 | 用途 | 许可 | 源码 / 说明 |
|------|------|------|-------------|
| mpv 0.42（单文件内嵌 `mpv.exe`） | 解码、渲染、JSON IPC 子进程 | GPLv2 或更高版本 | https://github.com/mpv-player/mpv |
| `d3dcompiler_43.dll` | mpv 运行时依赖，随单文件内嵌 | Microsoft Windows SDK 组件，按微软条款使用 | 随 Windows SDK 分发 |
| ONNX Runtime C API 头文件 | `third_party/onnxruntime/onnxruntime_c_api.h` | MIT License | https://github.com/microsoft/onnxruntime |
| FFmpeg / x264 | 仅构建与测试期生成素材，**不随发布物分发** | 各构建自带许可；发布物不含其代码 | https://ffmpeg.org |

## 关于 mpv 的边界

- 调用方式：独立进程 + 命名管道 JSON IPC，**未链接** libmpv，也未把 mpv 代码编入项目自有模块。
- 单文件发布物 `dist/LumenPlayer.exe` 把 `mpv.exe` 作为资源内嵌，首次运行释放到
  `%LOCALAPPDATA%\LumenPlayer\`。这一事实仍需遵守 mpv 的 GPLv2+ 条款，包括提供其源码获取途径。
- 播放器内 **右键 → 关于 LumenPlayer** 会显示 mpv 版本、许可与源码地址。
- 若需不披露源码的商业分发，须另行取得 mpv 权利人的许可，或替换为自购/自研解码组件。

## 注意

内嵌的 `d3dcompiler_43.dll` 为微软组件。若对随包分发有合规疑问，
应查阅微软 Windows SDK 的可再分发条款，或改用系统自带版本。
本项目不对该组件的再分发权利作任何声明。
