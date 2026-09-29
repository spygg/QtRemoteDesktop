#!/bin/bash
# ============================================================================
# QtRemoteDesktop Linux 一键编译脚本
# 与 Windows 构建对应：FFmpeg 3.4.8 + openh264 源码编译，产物全部输出到
#   <repo>/build    (cmake 中间目录)
#   <repo>/bin      (最终可执行文件)
#   <repo>/build_output  (第三方库产物: ffmpeg_install/openh264_install)
# src/ 目录保持零编译产物。
#
# 依赖：脚本会**自动检查**并打印适配当前发行版的安装命令（--check-deps 只检查不构建）。
# 必选：gcc/g++ make cmake pkg-config sed tar
#       Qt5 或 Qt6（必须带 WebSockets 组件）
#       X11 系列开发库：libX11 libXtst libXdamage libXcomposite libXrender libXfixes
#       openssl zlib alsa(asound) pcap 开发头文件
# 可选：libpipewire-0.3（Wayland 抓屏，缺失则仅有 X11 抓屏）
#       mlocate（Qt 探测加速，缺失则回退 qmake/目录扫描）
#       libdrm（RK3588 MPP 硬编）
#       libXrandr（进程内多显示器枚举；缺失则回退 `xrandr --query` 子进程枚举，
#                 多屏分辨率探测可能不稳，但编译与运行不受影响）
#
# 用法（零参数即可，Qt 自动探测）：
#   ./build_linux.sh                         # 自动探测 Qt
#   ./build_linux.sh -Q /opt/Qt/5.15.2/gcc_64 # 指定 Qt 前缀
#   ./build_linux.sh -j 8                    # 并行数
#   ./build_linux.sh --check-deps            # 只检查依赖并打印安装命令，不构建
#   ./build_linux.sh --clean                 # 全量重建（清空 build/ 与第三方产物）
#   ./build_linux.sh --skip-ffmpeg           # 强制跳过 FFmpeg 编译（已有产物复用；无产物则报错）
#
# 增量约定：默认**不删除**已有构建产物（build/、build_output/、ffmpeg_build/、
#   mpp_build/），只做增量编译；只有显式 --clean 才清空。
# ============================================================================

set -e

# 脚本可能位于 <repo>/src 或 <repo>/src/linux_build_script，统一按自身位置推算仓库根
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_DIR="$REPO_DIR/src"
BUILD_DIR="$REPO_DIR/build"
BIN_DIR="$REPO_DIR/bin"
OUT_DIR="$REPO_DIR/build_output"


find "$SRC_DIR" -type f \( -name "configure" -o -name "*.sh" \) -exec chmod +x {} \;


QT_PREFIX=""
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
SKIP_FFMPEG=0
CHECK_DEPS=0

usage() {
    echo "用法: $0 [-Q <Qt prefix>] [-j <jobs>] [--clean] [--skip-ffmpeg] [--check-deps]"
    echo "  -Q           Qt 安装前缀 (CMAKE_PREFIX_PATH)，默认自动探测"
    echo "  -j           并行编译数 (默认: nproc)"
    echo "  --clean      清空中间目录与构建产物后全量重建（默认增量，不删产物）"
    echo "  --skip-ffmpeg 强制跳过 FFmpeg 编译（已有产物复用；无产物则报错）"
    echo "  --check-deps  只检查依赖并打印安装命令，不构建"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -Q) QT_PREFIX="$2"; shift 2 ;;
        -j) JOBS="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        --skip-ffmpeg) SKIP_FFMPEG=1; shift ;;
        --check-deps) CHECK_DEPS=1; shift ;;
        -h|--help) usage ;;
        *) echo "未知参数: $1" >&2; usage ;;
    esac
done

# ---------------- Qt 自动探测 ----------------
detect_qt() {
    # ---------- 1. 用户已手动指定 ----------
    if [ -n "$QT_PREFIX" ]; then
        if [ ! -f "$QT_PREFIX/lib/cmake/Qt5/Qt5Config.cmake" ] && \
           [ ! -f "$QT_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ] && \
           [ ! -f "$QT_PREFIX/bin/qmake" ] && \
           [ ! -f "$QT_PREFIX/qmake" ]; then
            echo "错误: $QT_PREFIX 不是有效的 Qt 前缀（缺少 cmake 配置或 qmake）" >&2
            exit 1
        fi
        return
    fi

    # ---------- 2. 优先用 locate 找 cmake 配置 ----------
    if command -v locate >/dev/null 2>&1; then
        # 找 Qt5/Qt6 的 cmake 配置文件，按路径长度排序（短的通常是根安装）
        local cmake_file
        cmake_file=$(locate -b '\Qt5Config.cmake' '\Qt6Config.cmake' 2>/dev/null \
            | grep -E '/lib/cmake/Qt[56]/Qt[56]Config\.cmake$' \
            | awk '{ print length, $0 }' \
            | sort -n \
            | head -1 \
            | cut -d' ' -f2-)

        if [ -n "$cmake_file" ] && [ -f "$cmake_file" ]; then
            # 从 .../lib/cmake/Qt5/Qt5Config.cmake 推导出前缀
            # 例如 /opt/Qt/Qt5.12.8/lib/cmake/Qt5/Qt5Config.cmake → /opt/Qt/Qt5.12.8
            QT_PREFIX="${cmake_file%/lib/cmake/Qt*/Qt*Config.cmake}"
            if [ -d "$QT_PREFIX" ]; then
                echo "[qt] locate 探测到 Qt: $QT_PREFIX"
                return
            fi
        fi
    fi

    # ---------- 3. 回退：从 PATH 找 qmake ----------
    for q in qmake qmake6 qmake5 qmake-qt5 qmake-qt6; do
        local p
        if p=$(command -v "$q" 2>/dev/null); then
            QT_PREFIX=$(realpath "$(dirname "$p")/..")
            echo "[qt] 使用系统 Qt: $p -> $QT_PREFIX"
            return
        fi
    done

    # ---------- 4. 回退：遍历常见目录 ----------
    local candidates=(
        /opt/Qt/*/gcc_64
        /opt/Qt/*/
        /opt/Qt*/
        /usr/lib/qt5
        /usr/lib/qt6
        /usr/lib/x86_64-linux-gnu/qt5
        /usr/lib/x86_64-linux-gnu/qt6
        /usr/local/Qt*
    )

    for pattern in "${candidates[@]}"; do
        for dir in $pattern; do
            [ ! -d "$dir" ] && continue
            if [ -f "$dir/lib/cmake/Qt5/Qt5Config.cmake" ] || \
               [ -f "$dir/lib/cmake/Qt6/Qt6Config.cmake" ] || \
               [ -f "$dir/bin/qmake" ] || \
               [ -f "$dir/qmake" ]; then
                QT_PREFIX="$dir"
                echo "[qt] 探测到 Qt 前缀: $QT_PREFIX"
                return
            fi
        done
    done

    echo "错误: 未找到 Qt。请用 -Q 指定 Qt 前缀，或安装依赖：" >&2
    echo "  sudo apt install -y qtbase5-dev qtwebsockets5-dev" >&2
    echo "  sudo yum install -y alsa-lib-devel libpcap-devel openssl-devel zlib-devel" >&2
    exit 1
}




FFMPEG_SRC="$SRC_DIR/remotedesk/thridparty/ffmpeg/FFmpeg-n3.4.8"
OPENH264_SRC="$SRC_DIR/remotedesk/thridparty/third_party_src/openh264"
MPP_SRC="$SRC_DIR/remotedesk/thridparty/mpp"

FFMPEG_INSTALL="$OUT_DIR/ffmpeg_install"
OPENH264_INSTALL="$OUT_DIR/openh264_install"
MPP_INSTALL="$OUT_DIR/mpp_install"

step() { echo ""; echo ">>> $*"; }

# ---------------- 依赖检查与安装提示 ----------------
# 目标：把"编译到一半才报找不到 X11/XTest 头文件"提前到构建前，并按发行版
# 给出可直接复制的安装命令。

PKG_MGR="unknown"
PKG_INSTALL=""
DISTRO_FAMILY="未知"

detect_pkg_mgr() {
    if   command -v apt-get >/dev/null 2>&1; then PKG_MGR=apt;     PKG_INSTALL="sudo apt-get install -y"; DISTRO_FAMILY="Debian/Ubuntu 系"
    elif command -v dnf     >/dev/null 2>&1; then PKG_MGR=dnf;     PKG_INSTALL="sudo dnf install -y";     DISTRO_FAMILY="RedHat/Fedora/Anolis 系"
    elif command -v yum     >/dev/null 2>&1; then PKG_MGR=yum;     PKG_INSTALL="sudo yum install -y";     DISTRO_FAMILY="RedHat/CentOS 系"
    elif command -v zypper  >/dev/null 2>&1; then PKG_MGR=zypper;  PKG_INSTALL="sudo zypper install -y";  DISTRO_FAMILY="SUSE 系"
    elif command -v pacman  >/dev/null 2>&1; then PKG_MGR=pacman;  PKG_INSTALL="sudo pacman -S --needed"; DISTRO_FAMILY="Arch 系"
    elif command -v apk     >/dev/null 2>&1; then PKG_MGR=apk;     PKG_INSTALL="sudo apk add";           DISTRO_FAMILY="Alpine 系"
    else PKG_INSTALL="# 请手动安装"; DISTRO_FAMILY="未知（无法自动识别包管理器）"; fi
}

# 依赖 id -> 当前发行版的包名
pkg_for() {
    local id="$1" d="$PKG_MGR"
    case "$id" in
        toolchain) case "$d" in
            apt) echo "build-essential";; dnf|yum|zypper) echo "gcc gcc-c++ make";;
            pacman) echo "base-devel";; apk) echo "build-base";; *) echo "gcc g++ make";; esac ;;
        cmake)     echo "cmake" ;;
        pkgconfig) case "$d" in apt|dnf|yum|zypper) echo "pkg-config";; *) echo "pkgconf";; esac ;;
        X11) case "$d" in apt|apk) echo "libx11-dev";; dnf|yum|zypper) echo "libX11-devel";; pacman) echo "libx11";; *) echo "libX11-devel";; esac ;;
        Xtst) case "$d" in apt|apk) echo "libxtst-dev";; dnf|yum|zypper) echo "libXtst-devel";; pacman) echo "libxtst";; *) echo "libXtst-devel";; esac ;;
        Xdamage) case "$d" in apt|apk) echo "libxdamage-dev";; dnf|yum|zypper) echo "libXdamage-devel";; pacman) echo "libxdamage";; *) echo "libXdamage-devel";; esac ;;
        Xcomposite) case "$d" in apt|apk) echo "libxcomposite-dev";; dnf|yum|zypper) echo "libXcomposite-devel";; pacman) echo "libxcomposite";; *) echo "libXcomposite-devel";; esac ;;
        Xrender) case "$d" in apt|apk) echo "libxrender-dev";; dnf|yum|zypper) echo "libXrender-devel";; pacman) echo "libxrender";; *) echo "libXrender-devel";; esac ;;
        Xfixes) case "$d" in apt|apk) echo "libxfixes-dev";; dnf|yum|zypper) echo "libXfixes-devel";; pacman) echo "libxfixes";; *) echo "libXfixes-devel";; esac ;;
        ssl) case "$d" in apt) echo "libssl-dev";; dnf|yum|zypper) echo "openssl-devel";; pacman) echo "openssl";; apk) echo "openssl-dev";; *) echo "openssl-devel";; esac ;;
        zlib) case "$d" in apt) echo "zlib1g-dev";; pacman) echo "zlib";; apk) echo "zlib-dev";; *) echo "zlib-devel";; esac ;;
        alsa) case "$d" in apt) echo "libasound2-dev";; pacman) echo "alsa-lib";; apk) echo "alsa-lib-dev";; *) echo "alsa-lib-devel";; esac ;;
        pcap) case "$d" in apt) echo "libpcap-dev";; pacman) echo "libpcap";; apk) echo "libpcap-dev";; *) echo "libpcap-devel";; esac ;;
        qt) case "$d" in
            apt) echo "qtbase5-dev qtwebsockets5-dev";;
            dnf|yum) echo "qt5-qtbase-devel qt5-qtwebsockets-devel";;
            zypper) echo "qtbase5-devel libqt5-qtwebsockets-devel";;
            pacman) echo "qt5-base qt5-websockets";;
            apk) echo "qt5-qtbase-dev qt5-qtwebsockets-dev";;
            *) echo "Qt5/Qt6 (Core Gui Widgets Network WebSockets)";; esac ;;
        pipewire) case "$d" in apt) echo "libpipewire-0.3-dev";; dnf|yum) echo "pipewire-devel";; pacman) echo "pipewire";; apk) echo "pipewire-dev";; *) echo "libpipewire-0.3-dev";; esac ;;
        mlocate) case "$d" in apt) echo "mlocate";; pacman) echo "mlocate";; *) echo "mlocate";; esac ;;
        drm) case "$d" in apt) echo "libdrm-dev";; dnf|yum|zypper) echo "libdrm-devel";; pacman) echo "libdrm";; apk) echo "libdrm-dev";; *) echo "libdrm-devel";; esac ;;
        Xrandr) case "$d" in apt|apk) echo "libxrandr-dev";; dnf|yum|zypper) echo "libXrandr-devel";; pacman) echo "libxrandr";; *) echo "libXrandr-devel";; esac ;;
        *) echo "$id" ;;
    esac
}

# 依赖 id -> 用于探测的头文件（工具链类留空，只查命令）
hdr_for() {
    case "$1" in
        X11) echo "X11/Xlib.h" ;;
        Xtst) echo "X11/extensions/XTest.h" ;;
        Xdamage) echo "X11/extensions/Xdamage.h" ;;
        Xcomposite) echo "X11/extensions/Xcomposite.h" ;;
        Xrender) echo "X11/extensions/Xrender.h" ;;
        Xfixes) echo "X11/extensions/Xfixes.h" ;;
        ssl) echo "openssl/ssl.h" ;;
        zlib) echo "zlib.h" ;;
        alsa) echo "alsa/asoundlib.h" ;;
        pcap) echo "pcap/pcap.h" ;;
        drm) echo "libdrm/drm.h" ;;
        Xrandr) echo "X11/extensions/Xrandr.h" ;;
        *) echo "" ;;
    esac
}

have_header() { [ -n "$1" ] && echo "#include <$1>" | ${CXX:-g++} -E - >/dev/null 2>&1; }

# Qt 是否带 WebSockets 组件（工程强制依赖 Qt::WebSockets）
qt_has_websockets() {
    if [ -n "$QT_PREFIX" ]; then
        [ -d "$QT_PREFIX/lib/cmake/Qt5WebSockets" ] && return 0
        [ -d "$QT_PREFIX/lib/cmake/Qt6WebSockets" ] && return 0
        [ -f "$QT_PREFIX/include/QtWebSockets/qwebsocketserver.h" ] && return 0
        [ -f "$QT_PREFIX/include/x86_64-linux-gnu/qt5/QtWebSockets/qwebsocketserver.h" ] && return 0
        return 1
    fi
    # 未指定前缀时退化为全局探测，探测不到只作警告（可能装在非标准位置）
    pkg-config --exists Qt5WebSockets 2>/dev/null && return 0
    pkg-config --exists Qt6WebSockets 2>/dev/null && return 0
    return 1
}

MISSING_REQ=""
MISSING_OPT=""

check_deps() {
    step "依赖检查"
    detect_pkg_mgr
    echo "[deps] 系统发行系: ${DISTRO_FAMILY}（包管理器: ${PKG_MGR}；缺失依赖的安装命令前缀: ${PKG_INSTALL}）"

    local id
    # --- 工具链 ---
    local tool_ok=1
    for t in gcc g++ make cmake pkg-config sed tar; do
        if ! command -v "$t" >/dev/null 2>&1; then
            echo "  [缺失] 命令: $t"; tool_ok=0
        fi
    done
    [ $tool_ok -eq 0 ] && MISSING_REQ="$MISSING_REQ toolchain cmake pkgconfig"

    # --- 系统开发库（头文件探测）---
    for id in X11 Xtst Xdamage Xcomposite Xrender Xfixes ssl zlib alsa pcap; do
        if ! have_header "$(hdr_for "$id")"; then
            echo "  [缺失] 开发库: $id   (头文件 $(hdr_for "$id"))"
            MISSING_REQ="$MISSING_REQ $id"
        fi
    done

    # --- Qt（含 WebSockets）---
    if ! qt_has_websockets; then
        echo "  [缺失/未确认] Qt WebSockets 组件（工程必需）"
        [ -n "$QT_PREFIX" ] && MISSING_REQ="$MISSING_REQ qt"
    fi

    # --- 可选项 ---
    if pkg-config --exists libpipewire-0.3 2>/dev/null; then
        echo "  [可选] PipeWire: 已找到（Wayland 抓屏可用）"
    else
        echo "  [可选] PipeWire: 未找到（Wayland 抓屏将不可用，仅 X11 抓屏）"
        MISSING_OPT="$MISSING_OPT pipewire"
    fi
    # --- XRandR（可选：进程内多显示器枚举）---
    if have_header "$(hdr_for Xrandr)"; then
        echo "  [可选] XRandR: 已找到（进程内多显示器枚举可用）"
    else
        echo "  [可选] XRandR: 未找到（将回退 'xrandr --query' 子进程枚举，多屏分辨率探测可能不稳）"
        MISSING_OPT="$MISSING_OPT Xrandr"
    fi
    if ! command -v locate >/dev/null 2>&1; then
        echo "  [可选] locate: 未找到（Qt 探测会回退到 qmake/目录扫描）"
        MISSING_OPT="$MISSING_OPT mlocate"
    fi
    case "$(uname -m)" in
        aarch64|armv7l|armv6l|arm*)
            if ! have_header "$(hdr_for drm)"; then
                echo "  [可选] libdrm: 未找到（RK3588 MPP 硬编可能不可用）"
                MISSING_OPT="$MISSING_OPT drm"
            fi ;;
    esac

    # --- FFmpeg 源码（构建期内必需）---
    if [ $SKIP_FFMPEG -eq 0 ] && [ ! -d "$FFMPEG_SRC" ]; then
        echo "  [缺失] FFmpeg 源码: $FFMPEG_SRC"
        MISSING_REQ="$MISSING_REQ ffmpeg-src"
    fi

    # --- 汇总 ---
    if [ -n "$MISSING_REQ" ]; then
        echo ""
        echo "错误：缺少必需依赖，无法继续构建。" >&2
        local pkgs=""
        for id in $MISSING_REQ; do
            case "$id" in
                ffmpeg-src)
                    echo "  - FFmpeg 源码缺失：请解压 ffmpeg-3.4.8 到 $FFMPEG_SRC" >&2
                    continue ;;
                *) pkgs="$pkgs $(pkg_for "$id")" ;;
            esac
        done
        if [ -n "$pkgs" ]; then
            echo "" >&2
            echo "请在当前系统执行（$PKG_MGR）：" >&2
            echo "  $PKG_INSTALL $pkgs" >&2
        fi
        exit 1
    fi

    if [ -n "$MISSING_OPT" ]; then
        local optpkgs=""
        for id in $MISSING_OPT; do optpkgs="$optpkgs $(pkg_for "$id")"; done
        echo ""
        echo "[deps] 可选依赖未安装，功能会受限；如需完整功能可执行："
        echo "  $PKG_INSTALL $optpkgs"
    fi
    echo "[deps] 必需依赖检查通过"
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

# ---------------- 2. rockchip-mpp（RK3588 硬件编码，仅 ARM/Linux 构建）----
build_mpp() {
    # MPP 是 Rockchip 平台专用，x86/macOS 上跳过（不影响其他功能）
    case "$(uname -m)" in
        aarch64|armv7l|armv6l|arm*) ;;
        *) echo ">>> 非 ARM 平台，跳过 rockchip-mpp"; return ;;
    esac
    if [ ! -f "$MPP_SRC/CMakeLists.txt" ]; then
        echo ">>> 未找到 mpp 源码 $MPP_SRC，跳过 MPP 硬编（不影响其他功能）"
        return
    fi
    if [ -f "$MPP_INSTALL/lib/librockchip_mpp.a" ]; then
        echo ">>> rockchip-mpp 已存在 ($MPP_INSTALL/lib/librockchip_mpp.a)，跳过（增量构建）"
        return
    fi
    step "2/5 编译 rockchip-mpp -> $MPP_INSTALL"
    local mb="$BUILD_DIR/mpp_build"
    # 增量约定：默认复用已有构建目录做增量编译，仅 --clean 时清空
    if [ $CLEAN -eq 1 ]; then rm -rf "$mb"; fi
    mkdir -p "$mb" "$MPP_INSTALL"
    # BUILD_SHARED_LIBS=OFF 出静态库 librockchip_mpp_static.a；BUILD_TEST=OFF 跳过 demo
    ( cd "$mb" && cmake "$MPP_SRC" \
        -DBUILD_TEST=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$MPP_INSTALL" )
    ( cd "$mb" && make -j"$JOBS" && make install )
    # 头文件补拷（保险：确保 rk_mpi.h 等 public 头齐全）
    mkdir -p "$MPP_INSTALL/include/rockchip"
    cp -v "$MPP_SRC"/inc/*.h "$MPP_INSTALL/include/rockchip/" 2>/dev/null || true
    if [ ! -f "$MPP_INSTALL/lib/librockchip_mpp.a" ]; then
        echo "错误: rockchip-mpp 构建失败" >&2
        exit 1
    fi
}

# ---------------- 2.5 FFmpeg 3.4.8 ----------------
build_ffmpeg() {
    # 增量约定：产物已存在则跳过（configure+make 仅首次执行；如需全量重建用 --clean）。
    # 显式 --skip-ffmpeg 强制跳过；若产物尚不存在则报错（无法凭空复用）。
    if [ -f "$FFMPEG_INSTALL/lib/libavcodec.a" ]; then
        echo ">>> FFmpeg 已存在 ($FFMPEG_INSTALL/lib/libavcodec.a)，跳过（增量构建）"
        return
    fi
    if [ $SKIP_FFMPEG -eq 1 ]; then
        echo "错误: --skip-ffmpeg 指定但 FFmpeg 产物不存在: $FFMPEG_INSTALL" >&2
        exit 1
    fi
    # 按本机架构选择 FFmpeg 架构参数，避免 x86 专用的 mathops.h / emms 在 ARM 上误编
    case "$(uname -m)" in
        aarch64|arm64)  FFMPEG_ARCH="arm64" ;;
        armv7l|armv6l|arm*) FFMPEG_ARCH="arm" ;;
        x86_64|amd64)   FFMPEG_ARCH="x86_64" ;;
        i686|i386)      FFMPEG_ARCH="x86_32" ;;
        *)              FFMPEG_ARCH="$(uname -m)" ;;
    esac
    echo "[ffmpeg] 检测到架构: $(uname -m) -> --arch=$FFMPEG_ARCH"
    step "2/5 编译 FFmpeg 3.4.8 -> $FFMPEG_INSTALL"
    local ffbuild="$BUILD_DIR/ffmpeg_build"
    # 增量约定：默认复用已有构建目录（make 增量），仅 --clean 时清空。
    # 架构/前缀变化由下面的 configure 覆盖，无需删目录。
    if [ $CLEAN -eq 1 ]; then rm -rf "$ffbuild"; fi
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
        --cc=gcc --ld=gcc --arch="$FFMPEG_ARCH" \
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
        -DVENDOR_OPENSSL_102="$VENDOR_SSL102" \
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
    local exe="$BIN_DIR/QtRemoteDesktop"
    if [ ! -f "$exe" ]; then
        exe=$(find "$BIN_DIR" "$OUT_DIR" -name "QtRemoteDesktop" -type f 2>/dev/null | head -1)
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

detect_qt

# ---- 方案2：Qt5.9 自动启用 vendored OpenSSL 1.0.2u ----
# Qt5.9 的 QSslSocket 仅认 OpenSSL 1.0.x ABI；系统内若有 1.1.x（metaRTC DTLS 依赖）同进程
# 加载会导致 TLS 握手静默失败。Qt<5.10 时统一改链 vendored 1.0.2u（openssl102_install）。
VENDOR_SSL102=OFF
if [ -n "$QT_PREFIX" ] && [ -x "$QT_PREFIX/bin/qmake" ]; then
    QT_VER="$("$QT_PREFIX/bin/qmake" -query QT_VERSION 2>/dev/null)"
elif command -v qmake >/dev/null 2>&1; then
    QT_VER="$(qmake -query QT_VERSION 2>/dev/null)"
fi
if echo "$QT_VER" | grep -qE '^5\.[0-9]+\.'; then
    QT_MINOR="$(echo "$QT_VER" | cut -d. -f2)"
    if [ -n "$QT_MINOR" ] && [ "$QT_MINOR" -lt 10 ] 2>/dev/null; then
        VENDOR_SSL102=ON
        echo "[openssl] 检测到 Qt $QT_VER (<5.10)，自动启用 vendored OpenSSL 1.0.2u (VENDOR_OPENSSL_102=ON)"
    fi
fi

check_deps

if [ $CHECK_DEPS -eq 1 ]; then
    echo ""
    echo "[deps] 依赖检查完成（仅检查模式，未构建）"
    exit 0
fi

if [ $CLEAN -eq 1 ]; then
    step "清理旧构建"
    rm -rf "$BUILD_DIR" "$OUT_DIR"
    echo "[clean] 已清空 $BUILD_DIR 与 $OUT_DIR"
fi

cmake_configure
build_openh264_cmake
build_mpp
build_ffmpeg
# FFmpeg/MPP 在 configure 之后才编译，需重新 configure 让 FindFFmpeg/CMake 发现产物
cmake_configure
cmake_build
verify

echo ""
echo "=============================================="
echo " 一键编译完成！可执行文件: $BIN_DIR/QtRemoteDesktop"
echo "=============================================="
