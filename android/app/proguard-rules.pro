# JNI 方法由 native 侧按名字查找，混淆后名字变了就会 UnsatisfiedLinkError。
# 目前 release 未开 minify，这里先把规则写好，免得以后打开时踩坑。
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.lumen.player.NativeAnalyzer { *; }
