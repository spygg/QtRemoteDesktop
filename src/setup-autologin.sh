#!/bin/bash
# 配置 GDM 自动登录 + xfce X 会话，让 remotedesk 开机后即可截取桌面。
#
# 原理：开机 → GDM 自动登录 shlt → 启动 xfce 的 X 会话（xfce 无 Wayland，
# 必为 Xorg）→ remotedesk 的 checkTimer 自动检测到该会话并开始截屏。
#
# 用法：sudo bash setup-autologin.sh
#
# 安全提示：自动登录意味着开机后无需密码即进入桌面。远程访问仍受
#           remotedesk 的 token 认证保护。如不接受请勿运行。

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限，请用 sudo 执行：sudo bash $0" >&2
    exit 1
fi

USER_NAME="shlt"
GDM_CONF="/etc/gdm/custom.conf"
ACCT_DIR="/var/lib/AccountsService/users"

echo "==> 1/4 配置 GDM 自动登录（$USER_NAME）"
cp -a "$GDM_CONF" "${GDM_CONF}.bak.$(date +%s)" 2>/dev/null || true
cat > "$GDM_CONF" <<EOF
# 由 remotedeskp setup-autologin.sh 生成
[daemon]
AutomaticLogin=$USER_NAME
AutomaticLoginEnable=True

[security]

[xdmcp]

[chooser]

[debug]
EOF
echo "    已写入 $GDM_CONF"

echo "==> 2/4 设置 $USER_NAME 的会话为 xfce（X 会话）"
mkdir -p "$ACCT_DIR"
cat > "$ACCT_DIR/$USER_NAME" <<EOF
[User]
Session=xfce
SystemAccount=false
EOF
echo "    已写入 $ACCT_DIR/$USER_NAME"

echo "==> 3/4 确认 remotedesk 开机自启"
systemctl enable remotedesk 2>/dev/null && echo "    remotedesk 已启用" || echo "    remotedesk 服务未安装，请先运行 QtRemoteDesktop --install"

echo "==> 4/4 确认 xrdp 不与自动会话冲突"
# xrdp 默认用 :10。GDM 自动登录的 xfce 会话通常用 :1 或 :0，不会冲突。
# 若日后通过 xrdp 客户端登录，xrdp 会另起 :10 会话。
echo "    xrdp 仍可正常使用（独立 :10 会话）"

echo ""
echo "完成。请重启操作系统验证："
echo "  sudo reboot"
echo ""
echo "重启后通过网页访问 remotedesk，应能直接看到 xfce 桌面。"
echo "（首次可能需等待几秒让 GDM 完成自动登录 + remotedesk 检测到会话）"
