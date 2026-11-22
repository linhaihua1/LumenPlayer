package com.lumen.player

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.media3.common.MediaItem
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// ============================================================================
//  LumenPlayer Android - 主界面
//  ---------------------------------------------------------------------------
//  当前能力边界（和桌面版保持同一套说辞，避免两端承诺不一致）：
//    已实现：打开视频 → 播放 → 自动抽帧做场结构判定 → 把结论与建议显示出来
//    未实现：播放路径的画面处理（反交错/IVTC 滤镜）、AI 超分
//
//  为什么先做「判定」而不是先做「滤镜」：
//    判定是整条管线的入口，做错判定后面全是白费——把隔行片直接放大，
//    模型会把梳齿当纹理细节放大，比不处理更糟。判定跑通、且能被真实片源检验，
//    再上滤镜才有意义。
// ============================================================================
class MainActivity : AppCompatActivity() {

    private lateinit var player: ExoPlayer
    private lateinit var playerView: PlayerView
    private lateinit var resultText: TextView
    private lateinit var statusText: TextView

    private var currentUri: Uri? = null
    private var analyzeJob: Job? = null

    // SAF：不需要申请存储权限，用户选哪个文件我们就只能访问哪个。
    // 相比之下 READ_MEDIA_VIDEO 会拿到全部视频的访问权，对一个"播放器"来说过宽。
    private val pickVideo = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) openVideo(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        playerView = findViewById(R.id.playerView)
        resultText = findViewById(R.id.resultText)
        statusText = findViewById(R.id.statusText)

        player = ExoPlayer.Builder(this).build()
        playerView.player = player

        findViewById<Button>(R.id.openButton).setOnClickListener {
            pickVideo.launch(arrayOf("video/*"))
        }
        findViewById<Button>(R.id.reanalyzeButton).setOnClickListener {
            currentUri?.let { startAnalysis(it) }
        }

        // 原生库可用性先探一次：失败时立刻把原因写出来，
        // 而不是等用户拖进片子之后才看到一个空白的结论区。
        statusText.text = if (NativeAnalyzer.ensureLoaded()) {
            runCatching { NativeAnalyzer.nativeBuildInfo() }.getOrDefault("原生库已加载")
        } else {
            "原生库加载失败：${NativeAnalyzer.loadError}"
        }

        // 从别的应用"用 LumenPlayer 打开"进来
        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        val data = intent?.data ?: return
        openVideo(data)
    }

    private fun openVideo(uri: Uri) {
        currentUri = uri
        runCatching {
            player.setMediaItem(MediaItem.fromUri(uri))
            player.prepare()
            player.playWhenReady = true
        }.onFailure {
            statusText.text = "无法播放：${it.message}"
            return
        }
        startAnalysis(uri)
    }

    private fun startAnalysis(uri: Uri) {
        analyzeJob?.cancel()
        analyzeJob = lifecycleScope.launch {
            resultText.text = ""
            statusText.text = "正在准备分析…"

            val result = withContext(Dispatchers.IO) {
                SourceAnalyzer.analyze(this@MainActivity, uri) { msg ->
                    // 回调在 IO 线程，必须切回主线程改 UI
                    runOnUiThread { statusText.text = msg }
                }
            }
            render(result)
        }
    }

    private fun render(r: CadenceResult) {
        if (!r.valid) {
            statusText.text = "分析未完成"
            resultText.text = r.error ?: "有效样本不足（${r.frames}/${SourceAnalyzer.MIN_FRAMES} 帧）"
            return
        }

        val cadenceText = when (r.cadence) {
            "progressive" -> "真逐行"
            "interlaced" -> "真隔行"
            "telecine" -> "3:2 pulldown（电影转录）"
            "mixed" -> "混合（部分镜头隔行）"
            else -> "无法确定"
        }
        val orderText = when (r.fieldOrder) {
            "tff" -> "TFF（上优先）"
            "bff" -> "BFF（下优先）"
            "progressive" -> "逐行，无场序"
            else -> "未知"
        }
        val advice = when {
            r.ivtc -> "建议做 IVTC 还原 24p：删掉重复场后画质预算凭空多出约 20%，是收益最大的一步。"
            r.needsDeinterlace ->
                "建议启用反交错（bwdif / yadif，场序按 ${r.fieldOrder.uppercase()}）。" +
                    "不处理的话运动画面会出现横向梳齿。"
            r.cadence == "progressive" -> "无需场处理，可直接放大。"
            else -> "证据不足，建议换一段有运动的画面复核。"
        }

        val sb = StringBuilder()
        sb.append("片源　${r.width}×${r.height}　${formatDuration(r.durationMs)}\n")
        sb.append("判定　$cadenceText（置信度 ${(r.confidence * 100).toInt()}%）\n")
        sb.append("场序　$orderText\n")
        sb.append("建议　$advice\n")
        sb.append("依据　${r.reason}\n")
        sb.append("样本　有效 ${r.frames} 帧")
        if (r.dropped > 0) sb.append("，丢弃 ${r.dropped} 帧（尺寸不一致或解码失败）")
        sb.append("\n\n")
        sb.append("注：判定基于系统解码器输出的画面。个别机型硬解会自动去隔行，")
        sb.append("此时本机会把隔行片误判为逐行——这是当前方案的已知风险。")

        resultText.text = sb.toString()
        statusText.text = "分析完成"
    }

    private fun formatDuration(ms: Long): String {
        val total = ms / 1000
        return "%02d:%02d:%02d".format(total / 3600, (total % 3600) / 60, total % 60)
    }

    override fun onDestroy() {
        analyzeJob?.cancel()
        playerView.player = null
        player.release()
        super.onDestroy()
    }
}
