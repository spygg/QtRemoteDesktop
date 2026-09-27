#!/bin/bash
# ============================================================================
# QtRemoteDesktop Linux 构建入口（Qt5 / 兼容旧命令）
#
# 旧实现存在三个问题，已废弃并改为转发到 build_linux.sh：
#   1) 不解析 -Q 参数：QT_PREFIX 恒为空，detect_qt() 只能靠 locate/PATH 碰运气，
#      "指定 Qt 前缀"的用法实际不可用；
#   2) `cmake -S . -B build5` 在 src/ 目录内生成构建产物（src/build5），
#      污染源码树（项目约定 src/ 保持零编译产物）；
#   3) 未开启 -DBUILD_THIRDPARTY=ON：不编译 openh264/FFmpeg，产物只能跑 JPEG
#      图片模式，没有视频/WebRTC 能力。
#
# 现在等价于一键脚本 build_linux.sh，参数完全兼容：
#   ./linuxqt5.sh                          # 自动探测 Qt（Qt5/Qt6 均可）
#   ./linuxqt5.sh -Q /opt/Qt/5.15.2/gcc_64
#   ./linuxqt5.sh -j 8
#   ./linuxqt5.sh --check-deps             # 只检查依赖并打印安装命令
#   ./linuxqt5.sh --clean                  # 全量重建（默认增量）
#   ./linuxqt5.sh --skip-ffmpeg            # 跳过 FFmpeg 编译
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_SH="$SCRIPT_DIR/build_linux.sh"

if [ ! -f "$BUILD_SH" ]; then
    echo "错误: 找不到 $BUILD_SH" >&2
    exit 1
fi

echo "[qt5] 本脚本已合并到 build_linux.sh，以下参数将转发过去： $*"
exec "$BUILD_SH" "$@"
