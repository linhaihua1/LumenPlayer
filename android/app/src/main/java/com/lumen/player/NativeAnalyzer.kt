package com.lumen.player

// ============================================================================
//  JNI 入口（与 src/main/cpp/lumen_jni.cpp 一一对应）
//  ---------------------------------------------------------------------------
//  函数名规则：Java_<包名下划线化>_<类名>_<方法名>
//  这里类名是 NativeAnalyzer、方法名 analyzeFrames，
//  所以 C++ 侧必须是 Java_com_lumen_player_NativeAnalyzer_analyzeFrames。
//  改包名或类名时两边必须同步改，否则运行期才报 UnsatisfiedLinkError。
// ============================================================================
object NativeAnalyzer {

    private var loaded = false

    /** 加载失败原因；null 表示可用。 */
    var loadError: String? = null
        private set

    /**
     * 显式加载 .so。
     *
     * 不用 `init { System.loadLibrary(...) }` 的原因：object 初始化抛异常会变成
     * ExceptionInInitializerError，之后任何访问都炸，连错误提示都没法显示。
     * 这里把失败原因收起来，界面可以老老实实告诉用户「这台机器的 ABI 不在支持列表里」。
     */
    fun ensureLoaded(): Boolean {
        if (loaded) return true
        if (loadError != null) return false
        return try {
            System.loadLibrary("lumen_core_jni")
            loaded = true
            true
        } catch (t: Throwable) {
            loadError = t.message ?: "无法加载 liblumen_core_jni.so"
            false
        }
    }

    /**
     * 分析一组亮度平面（按时间顺序排列）。
     *
     * @return JSON：{ frames, dropped, cadence, confidence, fieldOrder,
     *                needsDeinterlace, ivtc, reason }
     */
    external fun analyzeFrames(frames: Array<ByteArray>, width: Int, height: Int): String

    /** 返回 core 的编译信息，用于确认 .so 真的加载上了。 */
    external fun nativeBuildInfo(): String
}
