#!/bin/bash
# 把手机壳打成 APK。只用命令行工具，不经过 gradle——
# gradle 会拉几百兆依赖，而我们这里只有两个源文件和一个 .so。
#
# 需要的三样东西（默认路径都在 /opt/android 下）：
#   build-tools   aapt2 / d8 / zipalign / apksigner
#   platform      android.jar，javac 编译时当 classpath
#   ndk           clang 交叉编译 .so
set -e

cd "$(dirname "$0")/.." || exit 1

SDK=${ANDROID_SDK:-/opt/android}
BT=${BUILD_TOOLS:-$SDK/android-14}
PLATFORM=${ANDROID_JAR:-$SDK/android-34/android.jar}
NDK=${ANDROID_NDK:-$SDK/android-ndk-r26d}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin

for f in "$BT/aapt2" "$PLATFORM" "$TOOLCHAIN/aarch64-linux-android23-clang"; do
    [ -e "$f" ] || { echo "缺少 $f"; exit 1; }
done

OUT=${1:-/workspace/简言.apk}
WORK=$(mktemp -d)
清理() { rm -rf "$WORK"; }
trap 清理 EXIT

echo "[1/5] 交叉编译 libjianyan.so"
# 两个 ABI：arm64-v8a 是真机主力，x86_64 让模拟器也能装。
# 同一份 C 源码编两次，只是换 target。
for abi in arm64-v8a x86_64; do
    case $abi in
        arm64-v8a) triple=aarch64-linux-android ;;
        x86_64)    triple=x86_64-linux-android ;;
    esac
    mkdir -p "$WORK/lib/$abi"
    "$TOOLCHAIN/${triple}23-clang" -shared -fPIC -O2 -s \
        -o "$WORK/lib/$abi/libjianyan.so" \
        手机/jni.c -lm
    printf "      %-11s %s KB\n" "$abi" "$(( $(stat -c%s "$WORK/lib/$abi/libjianyan.so") / 1024 ))"
done

echo "[2/5] aapt2 出基础包（manifest + assets）"
"$BT/aapt2" link -o "$WORK/base.apk" \
    -I "$PLATFORM" \
    --manifest 手机/app/AndroidManifest.xml \
    -A 手机/app/assets \
    --min-sdk-version 23 --target-sdk-version 34 \
    --version-code 17 --version-name 0.17

echo "[3/5] javac + d8"
mkdir -p "$WORK/classes" "$WORK/dex"
javac -source 11 -target 11 -classpath "$PLATFORM" -encoding UTF-8 \
    -d "$WORK/classes" 手机/app/java/ai/jianyan/MainActivity.java 2>/dev/null
"$BT/d8" --lib "$PLATFORM" --min-api 23 --output "$WORK/dex" \
    $(find "$WORK/classes" -name "*.class")

echo "[4/5] 组装 + 对齐"
cp "$WORK/base.apk" "$WORK/未签.apk"
# d8 把 dex 写在 dex/ 子目录里，这里先搬到包的根——zip 是按当前目录取名的，
# 少了这一步，APK 里就没有 classes.dex，装上一点就闪退。
cp "$WORK/dex/classes.dex" "$WORK/classes.dex"
( cd "$WORK" && zip -q -u 未签.apk classes.dex lib/arm64-v8a/libjianyan.so lib/x86_64/libjianyan.so )
# 装包前把内容念一遍：dex 和 so 少一个都是"能装、不能跑"
for f in classes.dex lib/arm64-v8a/libjianyan.so lib/x86_64/libjianyan.so; do
    unzip -l "$WORK/未签.apk" "$f" | grep -q "$f" || { echo "包里缺 $f"; exit 1; }
done
"$BT/zipalign" -f 4 "$WORK/未签.apk" "$WORK/对齐.apk"

echo "[5/5] 签名"
KS=手机/简言.jks
if [ ! -f "$KS" ]; then
    echo "      生成调试密钥（只用于自签测试，不要拿去发应用商店）"
    keytool -genkeypair -keystore "$KS" -alias jianyan \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass jianyan -keypass jianyan \
        -dname "CN=JianYan,O=JianYan,C=CN" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" \
    --ks-pass pass:jianyan --key-pass pass:jianyan \
    --min-sdk-version 23 \
    --out "$OUT" "$WORK/对齐.apk"

"$BT/apksigner" verify "$OUT"
echo
echo "好了：$OUT（$(du -h "$OUT" | cut -f1)）"
echo "安装：adb install -r $OUT"
