// ============================================================================
//  LumenPlayer Android - 工程设置
//  ---------------------------------------------------------------------------
//  仓库优先使用国内镜像：Google 的 dl.google.com 与 maven.google.com 在
//  部分网络环境下不可达（本项目实测直连超时），镜像放在最前面，官方源兜底。
// ============================================================================
pluginManagement {
    repositories {
        maven { url = uri("https://maven.aliyun.com/repository/gradle-plugin") }
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    // PREFER_SETTINGS：以本文件为准，避免模块里再出现第二份仓库配置导致行为不一致
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)
    repositories {
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        google()
        mavenCentral()
    }
}

rootProject.name = "LumenPlayer"
include(":app")
