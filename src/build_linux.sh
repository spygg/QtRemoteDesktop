#!/bin/bash
# ============================================================================
# QtRemoteDesktop Linux 一键编译脚本
# 与 Windows 构建对应：FFmpeg 3.4.8 + openh264 源码编译，产物全部输出到
#   <repo>/build    (cmake 中间目录)
#   <repo>/build_output  (最终产物: bin/lib/release + ffmpeg_install/openh264_install)
# src/ 目录保持零编译产物。
#
# 依赖（Ubuntu/Debian）：
#   sudo apt install -y gcc g++ make cmake pkg-config yasm nasm \
#     qtbase5-dev qt5-qmake qtwebchannel5-dev libx11-dev libxtst-dev \
#     libxdamage-dev libxcomposite-dev libxrender-dev libxfixes-dev \
#     libssl-dev zlib1g-dev libasound2-dev libpcap-dev
#
# 用法（零参数即可，Qt 自动探测）：
#   ./build_linux.sh                         # 自动探测 Qt
#   ./build_linux.sh -Q /opt/Qt/5.15.2/gcc_64 # 指定 Qt 前缀
#   ./build_linux.sh -j 8                    # 并行数
#   ./build_linux.sh --clean                 # 全量重建（清空 build/ 与构建产物）
#   ./build_linux.sh --skip-ffmpeg           # 跳过 FFmpeg 编译（复用已有产物）
# ============================================================================

set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_DIR/src"
BUILD_DIR="$REPO_DIR/build"
OUT_DIR="$REPO_DIR/build_output"


find "$SRC_DIR" -type f \( -name "configure" -o -name "*.sh" \) -exec chmod +x {} \;


QT_PREFIX=""
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
SKIP_FFMPEG=0

usage() {
    echo "用法: $0 [-Q <Qt prefix>] [-j <jobs>] [--clean] [--skip-ffmpeg]"
    echo "  -Q           Qt 安装前缀 (CMAKE_PREFIX_PATH)，默认自动探测"
    echo "  -j           并行编译数 (默认: nproc)"
    echo "  --clean      清空中间目录与构建产物后全量重建"
    echo "  --skip-ffmpeg 跳过 FFmpeg 编译，复用已有产物（只做主程序增量构建）"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -Q) QT_PREFIX="$2"; shift 2 ;;
        -j) JOBS="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        --skip-ffmpeg) SKIP_FFMPEG=1; shift ;;
        -h|--help) usage ;;
        *) echo "未知参数: $1" >&2; usage ;;
    esac
done

# ---------------- Qt 自动探测 ----------------
detect_qt() {
    if [ -n "$QT_PREFIX" ]; then
        if [ ! -f "$QT_PREFIX/bin/qmake" ]; then
            echo "错误: $QT_PREFIX/bin/qmake 不存在" >&2
            exit 1
        fi
        return
    fi
    for q in qmake qmake6 qmake5 qmake-qt5 qmake-qt6; do
        local p
        if p=$(command -v "$q" 2>/dev/null); then
            QT_PREFIX=$(realpath "$(dirname "$p")/..")
            echo "[qt] 使用系统 Qt: $p -> $QT_PREFIX"
            return
        fi
    done
    for dir in /opt/Qt/*/gcc_64 /opt/Qt/*/gcc_64/* /usr/lib/qt5 /usr/lib/qt6 /usr/lib/x86_64-linux-gnu/qt5 /usr/lib/x86_64-linux-gnu/qt6; do
        if [ -f "$dir/bin/qmake" ] || [ -f "$dir/lib/cmake/Qt5/Qt5Config.cmake" ] || [ -f "$dir/lib/cmake/Qt6/Qt6Config.cmake" ]; then
            QT_PREFIX="$dir"
            echo "[qt] 探测到 Qt 前缀: $QT_PREFIX"
            return
        fi
    done
    echo "错误: 未找到 Qt。请用 -Q 指定 Qt 前缀，或安装依赖：" >&2
    echo "  sudo apt install -y qtbase5-dev qt5-qmake qtwebchannel5-dev" >&2
	echo "  sudo yum install -y alsa-lib-devel libpcap-devel openssl-devel zlib-devel" >&2 

    exit 1
}




FFMPEG_SRC="$SRC_DIR/remotedesk/thridparty/ffmpeg/FFmpeg-n3.4.8"
OPENH264_SRC="$SRC_DIR/remotedesk/thridparty/third_party_src/openh264"

FFMPEG_INSTALL="$OUT_DIR/ffmpeg_install"
OPENH264_INSTALL="$OUT_DIR/openh264_install"

step() { echo ""; echo ">>> $*"; }

check_tools() {
    for t in gcc g++ make cmake pkg-config sed tar; do
        if ! command -v "$t" >/dev/null 2>&1; then
            echo "错误: 未找到 $t，请先安装构建工具链" >&2
            exit 1
        fi
    done
    for h in alsa/asoundlib.h pcap/pcap.h openssl/ssl.h zlib.h; do
        if ! echo '#include <'"$h"'>' | gcc -E - >/dev/null 2>&1; then
            echo "错误: 缺少系统开发头 $h" >&2
            echo "  sudo apt install -y libasound2-dev libpcap-dev libssl-dev zlib1g-dev" >&2
            exit 1
        fi
    done
    if [ ! -d "$FFMPEG_SRC" ]; then
        echo "错误: 找不到 FFmpeg 源码 $FFMPEG_SRC" >&2
        echo "请确认 ffmpeg-3.4.8.tar.bz2 已解压到 src/remotedesk/thridparty/ffmpeg/FFmpeg-n3.4.8" >&2
        exit 1
    fi
}

# ---------------- 1. openh264（由 CMake ExternalProject openh264_ep 构建）----
build_openh264_cmake() {
    if [ -f "$OPENH264_INSTALL/lib/libopenh264.a" ]; then
        echo ">>> openh264 已存在 ($OPENH264_INSTALL/lib/libopenh264.a)，跳过"
        return
    fi
    step "1/5 编译 openh264 (CMake ExternalProject)"
    cmake --build "$BUILD_DIR" --target openh264_ep -j"$JOBS"
    if [ ! -f "$OPENH264_INSTALL/lib/libopenh264.a" ]; then
        echo "错误: openh264 构建失败" >&2
        exit 1
    fi
}

# ---------------- 2. FFmpeg 3.4.8 ----------------
build_ffmpeg() {
    if [ -f "$FFMPEG_INSTALL/lib/libavcodec.a" ] && [ $SKIP_FFMPEG -eq 1 ]; then
        echo ">>> FFmpeg 已存在，跳过 ($FFMPEG_INSTALL/lib/libavcodec.a)"
        return
    fi
    step "2/5 编译 FFmpeg 3.4.8 -> $FFMPEG_INSTALL"
    local ffbuild="$BUILD_DIR/ffmpeg_build"
    rm -rf "$ffbuild"
    mkdir -p "$ffbuild" "$FFMPEG_INSTALL/lib" "$FFMPEG_INSTALL/include"
    # GCC 13+ 兼容补丁：x86/mathops.h 用 "ic" 内联汇编约束会触发
    # "operand type mismatch for shr"（GCC 生成非法 8 位移位操作数）。
    # 改为 "c"（强制 cl 寄存器，允许立即数），幂等，仅在首次 patch。
    local mops="$FFMPEG_SRC/libavcodec/x86/mathops.h"
    if grep -q ': "ic"' "$mops"; then
        sed -i 's/: "ic" ((uint8_t)(-s))/: "c" ((uint8_t)(-s))/g' "$mops"
        echo "[patch] mathops.h: ic -> c (GCC 13 兼容)"
    fi
    ( cd "$ffbuild" && PKG_CONFIG_PATH="$OPENH264_INSTALL/lib/pkgconfig" \
        "$FFMPEG_SRC/configure" \
        --cc=gcc --ld=gcc --arch=x86_64 \
        --disable-debug --disable-doc --disable-programs --disable-network \
        --disable-avformat --disable-avfilter --disable-avdevice \
        --disable-swresample --disable-postproc --disable-avresample \
        --disable-everything --enable-swscale --enable-libopenh264 \
        --enable-encoder=libopenh264 \
        --enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=mpeg4 \
        --disable-x86asm --disable-inline-asm --enable-static --disable-shared \
        --extra-cflags="-I$OPENH264_INSTALL/include" \
        --extra-ldflags="-L$OPENH264_INSTALL/lib" \
        --prefix="$FFMPEG_INSTALL" )
    ( cd "$ffbuild" && make -j"$JOBS" && make install )
    # 头文件补拷（make install 在带版本号源码目录名时可能漏装部分头）
    for lib in libavcodec libavutil libswscale; do
        mkdir -p "$FFMPEG_INSTALL/include/$lib"
        cp -v "$FFMPEG_SRC/$lib"/*.h "$FFMPEG_INSTALL/include/$lib/"
    done
    cp -v "$ffbuild/libavutil/avconfig.h" "$ffbuild/libavutil/ffversion.h" \
        "$FFMPEG_INSTALL/include/libavutil/" 2>/dev/null || true
    if [ ! -f "$FFMPEG_INSTALL/lib/libavcodec.a" ]; then
        echo "错误: FFmpeg 构建失败" >&2
        exit 1
    fi
}

# ---------------- 3. CMake 配置 ----------------
cmake_configure() {
    step "3/5 CMake 配置 (BUILD_THIRDPARTY=ON)"
    # 兼容 Windows 与 WSL 共享 build 目录：缓存中的源/构建路径与当前不一致时，
    # 丢弃旧缓存重建（CMake 无法在不同挂载路径下复用缓存）。
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        local need_clean=0
        if ! grep -q "$SRC_DIR" "$BUILD_DIR/CMakeCache.txt"; then
            need_clean=1
        fi
        # Windows 时代遗留的生成器（MinGW/Visual Studio）在 WSL 下无法复用
        if grep -qE "CMAKE_GENERATOR.*(MinGW|NMake|MSYS|Visual Studio)" "$BUILD_DIR/CMakeCache.txt"; then
            need_clean=1
        fi
        if grep -q "CMAKE_SYSTEM_NAME:INTERNAL=Windows" "$BUILD_DIR/CMakeCache.txt"; then
            need_clean=1
        fi
        if [ $need_clean -eq 1 ]; then
            echo "[cmake] 检测到旧缓存不兼容，清理后重新配置"
            rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
        fi
    fi
    local qarg=()
    if [ -n "$QT_PREFIX" ]; then
        qarg=("-DCMAKE_PREFIX_PATH=$QT_PREFIX")
    fi
    cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_THIRDPARTY=ON \
        "${qarg[@]}"
}

# ---------------- 4. 编译 ----------------
cmake_build() {
    step "4/5 CMake 构建"
    cmake --build "$BUILD_DIR" -j"$JOBS" --config Release
}

# ---------------- 5. 验证 ----------------
verify() {
    step "5/5 验证产物"
    local exe="$OUT_DIR/release/QtRemoteDesktop"
    if [ ! -f "$exe" ]; then
        exe=$(find "$OUT_DIR" -name "QtRemoteDesktop" -type f 2>/dev/null | head -1)
    fi
    if [ -z "$exe" ] || [ ! -f "$exe" ]; then
        echo "错误: 未找到可执行文件" >&2
        exit 1
    fi
    echo "可执行文件: $exe"
    if strings "$exe" 2>/dev/null | grep -q "Video encoder initialized"; then
        echo "[OK] Video 编码模式已启用"
    else
        echo "[警告] 未检测到 Video 编码模式，可能回退到 JPEG"
    fi
}

check_tools
detect_qt

if [ $CLEAN -eq 1 ]; then
    step "清理旧构建"
    rm -rf "$BUILD_DIR" "$OUT_DIR"
    echo "[clean] 已清空 $BUILD_DIR 与 $OUT_DIR"
fi

cmake_configure
build_openh264_cmake
build_ffmpeg
# FFmpeg 在 configure 之后才编译，需重新 configure 让 FindFFmpeg 发现产物
cmake_configure
cmake_build
verify

echo ""
echo "=============================================="
echo " 一键编译完成！可执行文件: $OUT_DIR/release/QtRemoteDesktop"
echo "=============================================="
