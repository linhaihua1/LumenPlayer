package com.lumen.player

import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadataRetriever
import android.net.Uri
import org.json.JSONObject
import kotlin.math.max

// ============================================================================
//  片源自检（Android 侧抽帧 + 原生判定）
//  ---------------------------------------------------------------------------
//  职责：把「抽帧」这件平台相关的事留在 Kotlin，把「判定」这件算法的事交给 core。
//
//  ★ 两个不能妥协的约束，写在这里是因为违反了不会有编译错误，只会悄悄判错：
//
//  1) 绝不能缩放帧。
//     梳齿（combing）是「相邻两行属于不同场」造成的单行尖刺。
//     一旦把 720x480 缩到 360x240，两行会被平均成一行，梳齿直接被抹平——
//     算法看到的是干净的逐行画面，结论必然是「逐行」。这是最容易犯的错。
//
//  2) 采样必须是「连续帧」，不能是「均匀撒点」。
//     3:2 pulldown 的判据是相邻帧之间的场重复模式（新/新/重/新/重）。
//     如果每 1 秒才取一帧，帧间隔变成 30 帧，这个周期根本不存在，
//     telecine 永远检测不到。所以这里取的是同一时间点起的连续 16 帧。
// ============================================================================

/** 判定结果。字段与 native 返回的 JSON 一一对应。 */
data class CadenceResult(
    val cadence: String,          // progressive / interlaced / telecine / mixed / unknown
    val confidence: Double,       // 0—1
    val fieldOrder: String,       // tff / bff / progressive / unknown
    val needsDeinterlace: Boolean,
    val ivtc: Boolean,            // 是否可还原 24p（telecine）
    val reason: String,
    val frames: Int,
    val dropped: Int,
    val width: Int,
    val height: Int,
    val durationMs: Long,
    val error: String? = null
) {
    val valid: Boolean get() = error == null && frames >= SourceAnalyzer.MIN_FRAMES

    /** 判定严重度：两段采样冲突时用它决定谁说了算。 */
    val severity: Int
        get() = when (cadence) {
            "telecine" -> 4
            "interlaced" -> 3
            "mixed" -> 2
            "progressive" -> 1
            else -> 0
        }
}

object SourceAnalyzer {

    /** core 侧 CadenceDetector 的最少有效帧数，必须与 C++ 保持一致。 */
    const val MIN_FRAMES = 12

    /** 单段采样帧数。留出 4 帧余量，个别帧抽失败也不至于跌破 MIN_FRAMES。 */
    private const val SAMPLE_FRAMES = 16

    /** 相邻采样帧的时间间隔：33ms ≈ 29.97fps 的一帧。 */
    private const val FRAME_STEP_US = 33_000L

    /**
     * 分析一个视频。
     *
     * @param onProgress 进度回调（会在 IO 线程被调用，调用方自行切主线程）
     */
    fun analyze(
        context: Context,
        uri: Uri,
        onProgress: ((String) -> Unit)? = null
    ): CadenceResult {
        if (!NativeAnalyzer.ensureLoaded()) {
            return failed("原生库加载失败：${NativeAnalyzer.loadError}", 0, 0, 0)
        }

        val retriever = MediaMetadataRetriever()
        try {
            retriever.setDataSource(context, uri)
            val durationMs = retriever
                .extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)
                ?.toLongOrNull() ?: 0L

            if (durationMs < 300) {
                return failed("片源太短（${durationMs}ms），无法判定", 0, 0, durationMs)
            }

            // 第一段放在 40%：避开片头台标、黑场、字幕板和片尾滚屏。
            onProgress?.invoke("正在抽帧（第一段）…")
            val first = sampleAndAnalyze(retriever, 0.40, durationMs, onProgress)

            // 只有「判成逐行」或「判不出来」时才补第二段。
            // 原因：静止镜头（人物特写、空镜）本身没有运动，梳齿和场重复都看不出来，
            // 单段采样很容易把它误判为逐行。换一个位置再测一次，取更"严重"的结论——
            // 对播放器来说，把隔行误判成逐行（放着梳齿不管）比反过来要糟糕得多。
            if (first != null && first.valid &&
                (first.cadence == "progressive" || first.cadence == "unknown")
            ) {
                onProgress?.invoke("第一段结论偏保守，换位置复核…")
                val second = sampleAndAnalyze(retriever, 0.75, durationMs, onProgress)
                if (second != null && second.valid && second.severity > first.severity) {
                    return second
                }
            }

            return first ?: failed("抽帧失败：系统解码器没有返回任何画面", 0, 0, durationMs)
        } catch (t: Throwable) {
            // 常见于：文件被删除、DRM 保护、编码格式本机不支持
            return failed("无法读取该文件：${t.message ?: t.javaClass.simpleName}", 0, 0, 0)
        } finally {
            runCatching { retriever.release() }
        }
    }

    /**
     * 从 duration 的 position 比例处起，连续抽 SAMPLE_FRAMES 帧并判定。
     * 返回 null 表示这一段的可用帧太少。
     */
    private fun sampleAndAnalyze(
        retriever: MediaMetadataRetriever,
        position: Double,
        durationMs: Long,
        onProgress: ((String) -> Unit)?
    ): CadenceResult? {
        val startUs = max(0L, (durationMs * position).toLong() * 1000L)

        var width = 0
        var height = 0
        val frames = ArrayList<ByteArray>(SAMPLE_FRAMES)
        var dropped = 0

        for (i in 0 until SAMPLE_FRAMES) {
            val tUs = startUs + i * FRAME_STEP_US
            val bmp = try {
                retriever.getFrameAtTime(tUs, MediaMetadataRetriever.OPTION_CLOSEST)
            } catch (t: Throwable) {
                null
            }
            if (bmp == null) {
                dropped++
                continue
            }

            if (width == 0) {
                width = bmp.width
                height = bmp.height
            }
            // 抽帧途中尺寸变化（可变分辨率码流）时丢弃，见 lumen_jni.cpp 的说明
            if (bmp.width != width || bmp.height != height) {
                dropped++
                bmp.recycle()
                continue
            }

            val luma = bitmapToLuma(bmp)
            bmp.recycle()
            if (luma == null) {
                dropped++
                continue
            }
            frames.add(luma)

            if (i % 4 == 0) onProgress?.invoke("正在抽帧 ${frames.size}/$SAMPLE_FRAMES …")
        }

        if (frames.isEmpty() || width <= 0 || height <= 0) return null

        onProgress?.invoke("正在分析场结构（${frames.size} 帧 ${width}x$height）…")
        val json = try {
            NativeAnalyzer.analyzeFrames(frames.toTypedArray(), width, height)
        } catch (t: Throwable) {
            return failed("原生分析失败：${t.message}", width, height, durationMs)
        }

        return parse(json, width, height, durationMs, dropped)
    }

    /**
     * Bitmap → 亮度平面（8bit，BT.601 studio swing）。
     *
     * 为什么用 601 而不是 709：老片源绝大多数是 601 编码，
     * 而 core 的黑场阈值（min_luma=20）也是按 studio swing 设的（黑=16、白=235）。
     * 用 full range 会让「黑边」变成 Y=0，虽然也能被判为暗，但亮度统计会整体偏移。
     */
    private fun bitmapToLuma(bmp: Bitmap): ByteArray? {
        val w = bmp.width
        val h = bmp.height
        if (w <= 0 || h <= 0) return null

        // HARDWARE bitmap 不能 getPixels，必须先拷成 ARGB_8888
        val argb = if (bmp.config == Bitmap.Config.ARGB_8888) {
            bmp
        } else {
            bmp.copy(Bitmap.Config.ARGB_8888, false) ?: return null
        }

        val px = IntArray(w * h)
        argb.getPixels(px, 0, w, 0, 0, w, h)
        val y = ByteArray(w * h)
        for (i in px.indices) {
            val c = px[i]
            val r = (c shr 16) and 0xFF
            val g = (c shr 8) and 0xFF
            val b = c and 0xFF
            y[i] = (((66 * r + 129 * g + 25 * b + 128) shr 8) + 16).toByte()
        }
        if (argb !== bmp) argb.recycle()
        return y
    }

    private fun parse(
        json: String,
        width: Int,
        height: Int,
        durationMs: Long,
        dropped: Int
    ): CadenceResult {
        return try {
            val o = JSONObject(json)
            val err = o.optString("error", "")
            if (err.isNotEmpty()) {
                return failed(err, width, height, durationMs)
            }
            CadenceResult(
                cadence = o.optString("cadence", "unknown"),
                confidence = o.optDouble("confidence", 0.0),
                fieldOrder = o.optString("fieldOrder", "unknown"),
                needsDeinterlace = o.optBoolean("needsDeinterlace", false),
                ivtc = o.optBoolean("ivtc", false),
                reason = o.optString("reason", ""),
                frames = o.optInt("frames", 0),
                dropped = dropped + o.optInt("dropped", 0),
                width = width,
                height = height,
                durationMs = durationMs
            )
        } catch (t: Throwable) {
            failed("判定结果解析失败：${t.message}", width, height, durationMs)
        }
    }

    private fun failed(msg: String, w: Int, h: Int, dur: Long) = CadenceResult(
        cadence = "unknown",
        confidence = 0.0,
        fieldOrder = "unknown",
        needsDeinterlace = false,
        ivtc = false,
        reason = "",
        frames = 0,
        dropped = 0,
        width = w,
        height = h,
        durationMs = dur,
        error = msg
    )
}
