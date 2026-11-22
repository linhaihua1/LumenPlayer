plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.lumen.player"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.lumen.player"
        minSdk = 24          // Android 7.0：Media3 的下限，且覆盖绝大多数在用设备
        targetSdk = 34
        versionCode = 1
        versionName = "0.2.0"

        // 只编真实存在的 ABI：x86 32 位已无实际设备，编了只是白占体积
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // c++_shared：core 只用到标准库，shared 版本避免每个 .so 各带一份 libc++
                arguments += listOf("-DANDROID_STL=c++_shared")
                cppFlags += listOf("-O2")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"   // 与 NDK r26/r27 自带版本一致
        }
    }

    buildTypes {
        debug {
            isJniDebuggable = true
        }
        release {
            isMinifyEnabled = false        // JNI 方法名不能被混淆，先不开混淆
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        // 保留调试符号，崩溃栈里才看得到 C++ 函数名
        jniLibs.keepDebugSymbols += "**/*.so"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")

    // 播放内核：Media3 / ExoPlayer
    // 为什么不用 libmpv：Android 上 libmpv 需要自己交叉编译 .so（mpv 官方不发 Android 包），
    // 且它的 Android 渲染要走 GL 纹理，整套接进来是「周」级工作量。
    // Media3 是官方维护、随 Maven 分发、硬解直通 SurfaceView，先让它把「能播」这件事做对。
    implementation("androidx.media3:media3-exoplayer:1.4.1")
    implementation("androidx.media3:media3-ui:1.4.1")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
