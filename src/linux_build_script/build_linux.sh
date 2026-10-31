#!/bin/bash
# ============================================================================
# QtRemoteDesktop Linux 一键编译脚本
# 与 Windows 构建对应：FFmpeg（默认 6.1.2，见 FFMPEG_VERSION）+ openh264 源码编译，
# 产物全部输出到
#   <repo>/build    (cmake 中间目录)
#   <repo>/bin      (最终可执行文件)
#   <repo>/build_output  (第三方库产物: ffmpeg_install/openh264_install)
# src/ 目录保持零编译产物。
#
# 编译成功后：自动把 onekeydeploy.sh 复制到可执行文件所在目录，并切换到该目录。
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




# ---- FFmpeg 版本 ----
# 默认 6.1.2：新驱动不再回答 NVENC API 8.0 的"预设 GUID"接口，3.4.8 的
# avcodec_open2 在这类机器上直接返回 -40（本机 RTX 5060 / 32.0.15.9071 实测），
# 4.1 起的 p1–p7 词表才可用；软编与 VAAPI 路径两个版本都正常。
# 老机器沿用 3.4.8：FFMPEG_VERSION=3.4.8 ./build_linux.sh。这里选定的版本会用
# -DFFMPEG_VERSION 显式传给 cmake（覆盖 CMakeLists 的按位宽默认），脚本与 CMake
# 用的源码目录、产物目录因此始终一致。
FFMPEG_VERSION="${FFMPEG_VERSION:-6.1.2}"
FFMPEG_MAJOR="${FFMPEG_VERSION%%.*}"

FFMPEG_SRC="$SRC_DIR/remotedesk/thridparty/ffmpeg/FFmpeg-n$FFMPEG_VERSION"
OPENH264_SRC="$SRC_DIR/remotedesk/thridparty/third_party_src/openh264"
MPP_SRC="$SRC_DIR/remotedesk/thridparty/mpp"

# 版本目录与本文件末尾传给 cmake 的 -DFFMPEG_VERSION 是同一份事实源：
# 老版本沿用 ffmpeg_install（已编好的机器不重编），新版本单独落盘，两版可共存。
if [ "$FFMPEG_MAJOR" -lt 5 ]; then
    FFMPEG_INSTALL="$OUT_DIR/ffmpeg_install"
else
    FFMPEG_INSTALL="$OUT_DIR/ffmpeg_install-$FFMPEG_VERSION"
fi
OPENH264_INSTALL="$OUT_DIR/openh264_install"
MPP_INSTALL="$OUT_DIR/mpp_install"

# ---- NVENC 外部头（仅 FFmpeg >= 5 需要）----
# 5.x 起源码树里没有 compat/nvenc，configure 只通过 pkg-config 找 ffnvcodec
# （要求 >= 12.1.14.0），因此需要仓库内的 nv-codec-headers-* 目录 + 一份
# ffnvcodec.pc。两者任一缺失 → NVENC 自动关闭退回软编，不影响构建成功。
FFNV_SEARCH_DIR="$SRC_DIR/remotedesk/thridparty/ffmpeg"

ff_is_legacy_ffmpeg() { [ "$FFMPEG_MAJOR" -lt 5 ]; }

# 打印可用的 ffnvcodec 头目录；不可用时返回非 0（不打印）。
# configure 的 check_pkg_config 一次带上 4 个头，缺一个就判不满足。
ff_nv_inc_dir() {
    local dir count
    dir="$(ls -d "$FFNV_SEARCH_DIR"/nv-codec-headers-* 2>/dev/null | sort -V | tail -1)"
    [ -n "$dir" ] || return 1
    [ -f "$dir/include/ffnvcodec/nvEncodeAPI.h" ] || return 1
    count="$(ls "$dir"/include/ffnvcodec/*.h 2>/dev/null | wc -l)"
    [ "$count" -ge 4 ] || return 1
    echo "$dir/include"
}

# nv-codec-headers-n12.2.72.0 -> 12.2.72.0（configure 拿它比对版本门槛）
ff_nv_version() {
    basename "${1%/include}" | sed 's/^nv-codec-headers-n\?//'
}

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
        vaapi) case "$d" in apt) echo "libva-dev libdrm-dev";; dnf|yum|zypper) echo "libva-devel libdrm-devel";; pacman) echo "libva libdrm";; apk) echo "libva-dev libdrm-dev";; *) echo "libva-devel libdrm-devel";; esac ;;
        Xrandr) case "$d" in apt|apk) echo "libxrandr-dev";; dnf|yum|zypper) echo "libXrandr-devel";; pacman) echo "libxrandr";; *) echo "libXrandr-devel";; esac ;;
        yasm) case "$d" in apt|dnf|yum|zypper|pacman|apk) echo "yasm";; *) echo "yasm";; esac ;;
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
        vaapi) echo "va/va.h" ;;
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
    # --- VAAPI（可选：x86 硬编；缺失时继续用 openh264 软编）---
    # FFmpeg 的 --enable-vaapi 由 configure 里的 check_lib 验证：头文件在但
    # 链接不到 -lva 时会自动禁用，不会让构建失败。这里用 libva.pc + 头文件
    # 双重判断，避免"有头文件没库"时每次构建都误判为需要重建 FFmpeg。
    if have_header "$(hdr_for vaapi)" && pkg-config --exists libva 2>/dev/null; then
        echo "  [可选] VAAPI: 已找到（FFmpeg 将启用 h264_vaapi/hevc_vaapi 硬编）"
    else
        echo "  [可选] VAAPI: 未找到（硬编不可用，视频编码使用 openh264 软编）"
        # 只装了 libva 运行时（桌面/驱动依赖带进来的）而没有 -devel 头包时，上面
        # 那句"未找到"很容易被误读成"我机器上明明有 libva"，然后反复确认 FFmpeg
        # 却编不出硬编。这里区分开，并把两个必要条件一次说全。
        if ldconfig -p 2>/dev/null | grep -q 'libva\.so\.'; then
            echo "           注意：本机有 libva 运行时但缺开发包 —— 需要装 $(pkg_for vaapi)"
            echo "           另：硬编还要求部署机有对应 GPU 的 VA-API 驱动（如 i965/radeonsi 的 *_video.so），"
            echo "           虚机（VMware/virtio 等）通常没有该驱动，装了开发包也仍然只能软编。"
        fi
        MISSING_OPT="$MISSING_OPT vaapi"
    fi
    # --- NVENC（可选：x86 NVIDIA 硬编；无需 CUDA Toolkit，驱动在运行时才要）---
    if ff_nvenc_probe; then
        echo "  [可选] NVENC: 可用（FFmpeg 将启用 h264_nvenc/hevc_nvenc，运行时需 NVIDIA 驱动）"
    elif ff_is_legacy_ffmpeg; then
        echo "  [可选] NVENC: 不可用（非 x86 平台，或源码内 compat/nvenc 头文件编译不过）"
    else
        # FFmpeg >= 5 不再自带 NVENC 头：缺仓库内的 nv-codec-headers 是最常见原因，
        # 说清楚比只报"编译不过"更容易自助排查。
        echo "  [可选] NVENC: 不可用（非 x86 平台，或 $FFNV_SEARCH_DIR 下没有 nv-codec-headers-*）"
        echo "           需要硬编时：解压 nv-codec-headers（FFmpeg $FFMPEG_VERSION 用 API >= 12.1.14）到该目录后重新构建"
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
    # x86/x86_64：FFmpeg x86asm 汇编优化（nasm/yasm）。缺汇编器时 ffmpeg configure
    # 自动禁用 x86asm 并退化为纯 C（sws 约慢 3x），这里提前提示安装，装上即自动启用。
    case "$(uname -m)" in
        x86_64|amd64|i686|i386)
            if ! command -v nasm >/dev/null 2>&1 && ! command -v yasm >/dev/null 2>&1; then
                echo "  [可选] nasm/yasm: 未找到（x86 FFmpeg 汇编优化将禁用，sws 约慢 3x）"
                MISSING_OPT="$MISSING_OPT yasm"
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
                    echo "  - FFmpeg 源码缺失：请把 ffmpeg-$FFMPEG_VERSION 解压到 $FFMPEG_SRC" >&2
                    echo "    （想用其他版本：FFMPEG_VERSION=3.4.8 $0，需该版本源码已就位）" >&2
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

# ---------------- 0. vendored OpenSSL 1.0.2u（Qt<5.10 ABI 对齐）------------
# 由 CMake ExternalProject openssl102_ep 构建，产物落在
#   $OUT_DIR/openssl102_install
# 注意：必须放在首次 cmake_configure 之后（configure 通过才能生成该目标），
# 但要在编译业务代码之前完成 —— metaRTC / webrtcsession 都要用它的头与 .so。
build_openssl102() {
    if [ "$VENDOR_SSL102" != "ON" ]; then
        return
    fi
    local libssl="$OUT_DIR/openssl102_install/lib/libssl.so.1.0.0"
    local libcrypto="$OUT_DIR/openssl102_install/lib/libcrypto.so.1.0.0"
    if [ -f "$libssl" ] && [ -f "$libcrypto" ]; then
        echo ">>> vendored OpenSSL 1.0.2u 已存在 ($libssl)，跳过（增量构建）"
        return
    fi
    step "0/5 编译 vendored OpenSSL 1.0.2u -> $OUT_DIR/openssl102_install"
    OPENSSL102_JOBS="$JOBS" cmake --build "$BUILD_DIR" --target openssl102_ep -j"$JOBS"
    if [ ! -f "$libssl" ] || [ ! -f "$libcrypto" ]; then
        echo "错误: vendored OpenSSL 1.0.2u 构建失败" >&2
        echo "  期望产物: $libssl" >&2
        echo "  详细日志: $BUILD_DIR/openssl102_build/src/make.log" >&2
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

# ---------------- 2.5 FFmpeg（版本见 FFMPEG_VERSION）----------------
# 硬编自动探测（都不需要用户手动加开关）：
#   VAAPI —— 要 libva 开发包（头文件 + pkg-config 记录，FFmpeg configure 还会
#            再验证一次能否链接 -lva/-lva-drm）；只有运行时 libva.so 不够。
#   NVENC —— 不需要 CUDA Toolkit，运行时才 dlopen 驱动（libnvencodeapi.so*），
#            且仅在 x86 平台受支持（configure 里非 x86 会强制 disable nvenc）。
#            头文件来源随版本不同：3.4.8 自带 compat/nvenc；>=5 必须用仓库内的
#            nv-codec-headers，并且 configure 只通过 pkg-config 认它（下方
#            ff_write_ffnvcodec_pc 生成 .pc 并加进 PKG_CONFIG_PATH）。
# 探测方式与 configure 自身的检查保持一致，探测通过才把开关传进 configure，
# 避免出现"显式 --enable-xxx 但环境不满足 → configure 直接报错中断构建"。
ff_nvenc_probe() {
    # 与 FFmpeg configure 自己的门槛一致：x86 家族 + 头文件可编译。
    # 位宽/驱动是否真能用交给运行时（videoencoder 的 findHwEncoder 逐个候选 open，
    # 失败就 blockHwBackend 退回软编）：新驱动的 32 位 libcuda/nvcuda 只剩占位实现，
    # 而老显卡+老驱动的 32 位环境仍能硬编，构建期一刀切会把后者永久挡掉。
    case "$(uname -m)" in
        x86_64|amd64|i686|i386) ;;
        *) return 1 ;;
    esac
    local probe rc
    local -a inc_args=()
    probe="$(mktemp "${TMPDIR:-/tmp}/qtrd_nvenc_probe_XXXXXX.c")" || return 1
    if ff_is_legacy_ffmpeg; then
        cat > "$probe" <<'EOF'
#include "compat/nvenc/nvEncodeAPI.h"
NV_ENCODE_API_FUNCTION_LIST flist;
void f(void) { struct { const GUID guid; } s[] = { { NV_ENC_PRESET_HQ_GUID } }; }
int main(void) { return 0; }
EOF
        inc_args=(-I"$FFMPEG_SRC")
    else
        # 新版头里已经没有 NV_ENC_PRESET_*_GUID（那正是 3.4.8 在新驱动上打不开的
        # 原因），探测样例改用 NV_ENC_CODEC_H264_GUID —— 与 configure 的自检一致。
        local nv_inc
        nv_inc="$(ff_nv_inc_dir)" || { rm -f "$probe"; return 1; }
        cat > "$probe" <<'EOF'
#include <ffnvcodec/nvEncodeAPI.h>
NV_ENCODE_API_FUNCTION_LIST flist;
void f(void) { struct { const GUID guid; } s[] = { { NV_ENC_CODEC_H264_GUID } }; }
int main(void) { return 0; }
EOF
        inc_args=(-I"$nv_inc")
    fi
    ${CC:-gcc} "${inc_args[@]}" -c "$probe" -o /dev/null >/dev/null 2>&1
    rc=$?
    rm -f "$probe"
    return $rc
}

# FFmpeg >= 5 的 configure 认 ffnvcodec 只走 pkg-config（没有 --extra-cflags 的
# 旁路），所以给它一份 .pc：版本取自目录名，Cflags 指向头目录。
ff_write_ffnvcodec_pc() {
    local pkgdir="$1" inc ver
    if ff_is_legacy_ffmpeg; then return 0; fi
    inc="$(ff_nv_inc_dir)" || return 1
    ver="$(ff_nv_version "$inc")"
    mkdir -p "$pkgdir"
    cat > "$pkgdir/ffnvcodec.pc" <<EOF
prefix=${inc}/..
includedir=\${prefix}

Name: ffnvcodec
Description: FFmpeg version of Nvidia Codec SDK headers
Version: ${ver}
Cflags: -I${inc}
EOF
}

build_ffmpeg() {
    # 构建目录随版本分开：切版本时不会读到上一个版本的 config.h（增量判断靠它）
    local ffbuild
    if ff_is_legacy_ffmpeg; then
        ffbuild="$BUILD_DIR/ffmpeg_build"
    else
        ffbuild="$BUILD_DIR/ffmpeg_build-$FFMPEG_VERSION"
    fi
    local pkgconfig_dir="$BUILD_DIR/ffmpeg_pkgconfig"
    local vaapi_marker="$ffbuild/.vaapi_requested"
    local nvenc_marker="$ffbuild/.nvenc_requested"
    # libva 是可选依赖：有头文件且有 pkg-config 记录才启用 VAAPI。FFmpeg 的
    # configure 还会用 check_lib(-lva/-lva-drm) 再验证一次，缺库时自动降级。
    local want_vaapi=0
    if have_header "$(hdr_for vaapi)" && pkg-config --exists libva 2>/dev/null; then
        want_vaapi=1
    fi
    local want_nvenc=0
    if ff_nvenc_probe; then
        want_nvenc=1
    fi

    # 增量约定：产物已存在则跳过（configure+make 仅首次执行；如需全量重建用 --clean）。
    # 显式 --skip-ffmpeg 强制跳过；若产物尚不存在则报错（无法凭空复用）。
    if [ $SKIP_FFMPEG -eq 1 ]; then
        if [ -f "$FFMPEG_INSTALL/lib/libavcodec.a" ]; then
            echo ">>> --skip-ffmpeg：复用已有 FFmpeg ($FFMPEG_INSTALL/lib/libavcodec.a)"
            return
        fi
        echo "错误: --skip-ffmpeg 指定但 FFmpeg 产物不存在: $FFMPEG_INSTALL" >&2
        exit 1
    fi
    if [ -f "$FFMPEG_INSTALL/lib/libavcodec.a" ]; then
        local have_vaapi=0 have_nvenc=0
        if [ -f "$ffbuild/config.h" ]; then
            grep -q '^#define CONFIG_H264_VAAPI_ENCODER 1' "$ffbuild/config.h" && have_vaapi=1
            grep -q '^#define CONFIG_H264_NVENC_ENCODER 1' "$ffbuild/config.h" && have_nvenc=1
        fi
        # 任一硬编功能"现在探测得到、但已有产物里没编进去、且上次没失败过"→ 必须
        # 全量重配（config.h、对象文件与 Makefile 都已过时，仅 make 增量可能链接到缺符号的目标）。
        local need_reconfig=0
        if [ $want_vaapi -eq 1 ] && [ $have_vaapi -eq 0 ] && [ ! -f "$vaapi_marker" ]; then
            echo ">>> 检测到 libva，但已有 FFmpeg 未启用 VAAPI，重新配置并构建"
            need_reconfig=1
        fi
        if [ $want_nvenc -eq 1 ] && [ $have_nvenc -eq 0 ] && [ ! -f "$nvenc_marker" ]; then
            echo ">>> 本机支持 NVENC，但已有 FFmpeg 未启用 NVENC，重新配置并构建"
            need_reconfig=1
        fi
        if [ $need_reconfig -eq 1 ]; then
            rm -rf "$ffbuild"
        else
            [ $want_vaapi -eq 1 ] && [ $have_vaapi -eq 0 ] && \
                echo ">>> [warn] 已有 FFmpeg 未启用 VAAPI（上次尝试未成功），继续复用产物"
            [ $want_nvenc -eq 1 ] && [ $have_nvenc -eq 0 ] && \
                echo ">>> [warn] 已有 FFmpeg 未启用 NVENC（上次尝试未成功），继续复用产物"
            if [ \( $want_vaapi -eq 1 -a $have_vaapi -eq 0 \) -o \( $want_nvenc -eq 1 -a $have_nvenc -eq 0 \) ]; then
                echo "           如需重试：修好依赖后执行 --clean 重新构建"
            else
                echo ">>> FFmpeg 已存在 ($FFMPEG_INSTALL/lib/libavcodec.a)，跳过（增量构建）"
            fi
            return
        fi
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
    step "2/5 编译 FFmpeg $FFMPEG_VERSION -> $FFMPEG_INSTALL"
    # 增量约定：默认复用已有构建目录（make 增量），仅 --clean 时清空。
    # 架构/前缀变化由下面的 configure 覆盖，无需删目录。
    if [ $CLEAN -eq 1 ]; then rm -rf "$ffbuild"; fi
    mkdir -p "$ffbuild" "$FFMPEG_INSTALL/lib" "$FFMPEG_INSTALL/include"
    # 硬编开关只在本次配置时按探测结果启用；各自的 marker 记录"已尝试过"，
    # 防止某些发行版 libva 头/库不匹配（或探测与 configure 判定不一致）导致
    # 每次构建都反复删目录重编（几十次构建后才发现的构建时间黑洞）。
    local ff_hw_flags=""
    if [ $want_vaapi -eq 1 ]; then
        ff_hw_flags="$ff_hw_flags --enable-vaapi --enable-encoder=h264_vaapi --enable-encoder=hevc_vaapi"
        echo "[ffmpeg] 启用 VAAPI 硬编: h264_vaapi / hevc_vaapi"
    else
        echo "[ffmpeg] 未检测到 libva，VAAPI 硬编禁用（视频编码走 openh264 软编）"
    fi
    if [ $want_nvenc -eq 1 ]; then
        ff_hw_flags="$ff_hw_flags --enable-nvenc --enable-encoder=h264_nvenc --enable-encoder=hevc_nvenc"
        echo "[ffmpeg] 启用 NVENC 硬编: h264_nvenc / hevc_nvenc（运行时需 NVIDIA 驱动）"
    elif ff_is_legacy_ffmpeg; then
        echo "[ffmpeg] 非 x86 或 compat/nvenc 头文件不可编译，NVENC 禁用"
    else
        echo "[ffmpeg] 非 x86 或缺 nv-codec-headers（FFmpeg $FFMPEG_VERSION 的外部 NVENC 头），NVENC 禁用"
    fi
    # GCC 13+ 兼容补丁：x86/mathops.h 用 "ic" 内联汇编约束会触发
    # "operand type mismatch for shr"（GCC 生成非法 8 位移位操作数）。
    # 改为 "c"（强制 cl 寄存器，允许立即数），幂等，仅在首次 patch。
    local mops="$FFMPEG_SRC/libavcodec/x86/mathops.h"
    if grep -q ': "ic"' "$mops"; then
        sed -i 's/: "ic" ((uint8_t)(-s))/: "c" ((uint8_t)(-s))/g' "$mops"
        echo "[patch] mathops.h: ic -> c (GCC 13 兼容)"
    fi
    # GCC 14 兼容：把 implicit-function-declaration / int-conversion /
    # incompatible-pointer-types 等老 C 代码告警从 error 降回 warning（FFmpeg 3.4.8 太老）。
    # 各 flag 是否存在随 GCC 版本不同（如 return-mismatch 仅 GCC14+），逐个探测避免硬失败。
    local gcc14_flags=""
    for f in -Wno-error=implicit-function-declaration -Wno-error=implicit-int \
             -Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
             -Wno-error=return-mismatch; do
        if echo 'int main(void){return 0;}' | ${CC:-gcc} "$f" -x c - -c -o /dev/null >/dev/null 2>&1; then
            gcc14_flags="$gcc14_flags $f"
        fi
    done
    # x86 必须保留 NASM 汇编：libswscale 的 RGB32->YUV420P 有 SSE 实现，
    # 实测 1920x1200 单线程 11.5ms（纯 C）→ 3.9ms（asm），8 条带 3.3→1.2ms，
    # 折算 60fps 下每帧省 ~2ms、约 0.13 核。此前无条件 --disable-x86asm 把
    # x86 也退化成纯 C，是 192.168.1.12 上 CPU 偏高的直接原因之一。
    # ARM 没有 x86 asm，该开关无影响；仅当 x86 且工具链缺 nasm/yasm 时才关闭。
    local ffasm_flag="--disable-x86asm"
    case "$FFMPEG_ARCH" in
        x86_64|x86_32)
            if command -v nasm >/dev/null 2>&1 || command -v yasm >/dev/null 2>&1; then
                ffasm_flag=""
            else
                echo "[warn] 未安装 nasm/yasm，x86 FFmpeg 将退化为纯 C（sws 约慢 3x）" >&2
            fi
            ;;
    esac
    # 内联汇编（inline asm）不再无条件禁用：
    #   - ARM/aarch64：ffmpeg 的 NEON 优化（h264 解码/缩放等）完全依赖内联汇编
    #     （arm 无外部汇编器机制），此前 --disable-inline-asm 把 ARM 也退化成纯 C，
    #     实测解码性能损失明显；
    #   - x86：mathops.h 的 GCC13 "ic" 约束问题已由上方 sed patch 修复，GCC14
    #     告警由下方 gcc14_flags 兜底，内联汇编可作为 x86asm 的补充启用。
    # 启用后若个别老 GCC 版本出现汇编语法错误，configure 日志会指明具体文件，
    # 届时按文件做等价 patch 即可（不要整平台退回纯 C）。
    local ff_inline_asm_flag=""
    # avresample 在 FFmpeg 5.0 里已被删除，>=5 的 configure 不认这个开关（给了就
    # 直接报错），所以只有老分支带它。
    local ff_avresample_flag="--disable-avresample"
    ff_is_legacy_ffmpeg || ff_avresample_flag=""
    # FFmpeg >= 5 通过 pkg-config 认 NVENC 头（ffnvcodec），把 .pc 写进本仓库目录
    # 并排在 PKG_CONFIG_PATH 最前；老版本这里什么都不做，仍只吃 openh264 的 .pc。
    if [ $want_nvenc -eq 1 ] && ! ff_write_ffnvcodec_pc "$pkgconfig_dir"; then
        echo "[warn] NVENC 探测通过但写不出 ffnvcodec.pc，本次不启用 NVENC" >&2
        ff_hw_flags="${ff_hw_flags/--enable-nvenc/}"
        want_nvenc=0
    fi
    ( cd "$ffbuild" && PKG_CONFIG_PATH="$pkgconfig_dir:$OPENH264_INSTALL/lib/pkgconfig" \
        "$FFMPEG_SRC/configure" \
        --cc=gcc --ld=gcc --arch="$FFMPEG_ARCH" \
        --disable-debug --disable-doc --disable-programs --disable-network \
        --disable-avformat --disable-avfilter --disable-avdevice \
        --disable-swresample --disable-postproc $ff_avresample_flag \
        --disable-everything --enable-swscale --enable-libopenh264 \
        --enable-encoder=libopenh264 \
        --enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=mpeg4 \
        $ff_hw_flags \
        $ffasm_flag $ff_inline_asm_flag --enable-static --disable-shared \
        --extra-cflags="-I$OPENH264_INSTALL/include $gcc14_flags" \
        --extra-ldflags="-L$OPENH264_INSTALL/lib" \
        --prefix="$FFMPEG_INSTALL" )
    ( cd "$ffbuild" && make -j"$JOBS" && make install )
    # 探测通过但 configure 仍把某个硬编关掉（发行版头/库版本不匹配等）：写 marker
    # 记录"已尝试"，避免下次构建又因"产物未启用硬编"删目录全量重编（构建时间黑洞）。
    local cfg="$ffbuild/config.h"
    if [ $want_vaapi -eq 1 ] \
       && ! grep -q '^#define CONFIG_H264_VAAPI_ENCODER 1' "$cfg" 2>/dev/null; then
        : > "$vaapi_marker"
        echo "[warn] libva 已安装但 FFmpeg 未能启用 h264_vaapi（头/库版本不匹配）" >&2
        echo "       继续使用软编；修好 libva 后执行 --clean 重新构建即可启用硬编" >&2
    fi
    if [ $want_nvenc -eq 1 ] \
       && ! grep -q '^#define CONFIG_H264_NVENC_ENCODER 1' "$cfg" 2>/dev/null; then
        : > "$nvenc_marker"
        echo "[warn] NVENC 探测通过但 FFmpeg 未能启用 h264_nvenc" >&2
        echo "       继续使用软编；执行 --clean 重新构建可重试" >&2
    fi
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
        -DFFMPEG_VERSION="$FFMPEG_VERSION" \
        "${qarg[@]}"
}

# ---------------- 4. 编译 ----------------
cmake_build() {
    step "4/5 CMake 构建"
    cmake --build "$BUILD_DIR" -j"$JOBS" --config Release
}

# ---------------- 5. 验证 ----------------
EXE_PATH=""
EXE_DIR=""

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
    EXE_PATH="$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")"
    EXE_DIR="$(cd "$(dirname "$exe")" && pwd)"
    echo "可执行文件: $EXE_PATH"
    if strings "$exe" 2>/dev/null | grep -q "Video encoder initialized"; then
        echo "[OK] Video 编码模式已启用"
    else
        echo "[警告] 未检测到 Video 编码模式，可能回退到 JPEG"
    fi
}

# ---------------- 6. 复制 onekeydeploy.sh 到可执行目录 ----------------
# 编译成功后，把一键打包脚本放到可执行文件旁边，方便直接执行：
#   cd <可执行目录> && ./onekeydeploy.sh QtRemoteDesktop
deploy_onekey() {
    local src="$SCRIPT_DIR/onekeydeploy.sh"
    if [ ! -f "$src" ]; then
        echo "[警告] 未找到 $src，跳过复制" >&2
        return 0
    fi
    mkdir -p "$EXE_DIR"
    cp -f "$src" "$EXE_DIR/onekeydeploy.sh"
    chmod +x "$EXE_DIR/onekeydeploy.sh"
    echo "[deploy] onekeydeploy.sh 已复制到: $EXE_DIR/onekeydeploy.sh"
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
build_openssl102
build_openh264_cmake
build_mpp
build_ffmpeg
# FFmpeg/MPP 在 configure 之后才编译，需重新 configure 让 FindFFmpeg/CMake 发现产物
cmake_configure
cmake_build
verify

# 编译成功（verify 通过）后：复制 onekeydeploy.sh 并切换到可执行目录
deploy_onekey
cd "$EXE_DIR" || exit 1
echo "已切换到可执行目录: $EXE_DIR"

echo ""
echo "=============================================="
echo " 一键编译完成！可执行文件: $EXE_PATH"
echo " 打包脚本: $EXE_DIR/onekeydeploy.sh  (用法: ./onekeydeploy.sh $(basename "$EXE_PATH"))"
echo "=============================================="
