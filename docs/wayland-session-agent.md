# Linux Wayland 会话代理：终态说明

> 适用范围：Linux + Wayland（GNOME/mutter 实测）上的「root 系统服务 + 用户会话代理」双进程形态。
> 状态：Mutter 路径与 portal 回退路径均已在 192.168.1.53（Anolis 8 / GNOME Wayland / 800×600）实测；portal 补验的四项修复与文档校正随 v1.5.1 提交。

## 1. 形态与职责

```
remotedesk.service (system, root)
  ├─ 端口 / 配置 / 认证 / 编码 / 锁屏判定
  ├─ 采集优先链：会话代理 → PipeWire(Wayland) → X11 → Xvnc
  └─ 虚拟显示回落（代理接管期间不常驻）

remotedesk-agent.service (user, 桌面用户, 随图形会话启动)
  ├─ PipeWire ScreenCast 采集真实用户桌面（Mutter ScreenCast 优先，portal 回退）
  ├─ 输入注入：优先会话内 /dev/uinput（绝对指针 + 虚拟键盘）
  └─ 与控制端通过 unix socket + /dev/shm 双槽交换帧与输入事件
```

- 控制通道：`$XDG_RUNTIME_DIR/remotedesk/session-agent.sock`（目录 0700、socket 0600），换行分隔 JSON。
- 帧通道：`/dev/shm/remotedesk-agent.<uid>.<pid>`，双槽 RGB32，ack 复用；两槽都未 ack 时丢帧而不是覆盖正在拷贝的像素。
- 坐标基准：**只以代理上报的宽高为准**，root 侧不叠加任何补偿。
- 输入降级：`/dev/uinput` 不可写时，代理在 hello 中报 `input=none`，root 侧改用自身注入器；install/启动日志打印 `usermod -aG input <user>`，不修改系统权限。

## 2. 改动文件

新增（Wayland 双进程相关）

- `src/remotedesk/server/sessionagent/session_agent_protocol.h` — 协议常量（socket 路径、槽数、帧上限、版本号）
- `src/remotedesk/server/sessionagent/session_agent_client.h/.cpp` — root 侧单例客户端 + `ExternalInjector` 实现
- `src/remotedesk/server/sessionagent/session_agent_run.h/.cpp` — 代理进程入口（`--session-agent`）
- `src/remotedesk/server/screencapturer/screencapturer_agent.h/.cpp` — root 侧「代理帧」采集器
- `src/remotedesk/server/inputmanger/external_injector.h` — 注入器接口（header-only）

修改

- `src/remotedesk/server/screencapturer/screencapturer.h` / `screencapturer_linux.cpp` / `screencapturer_wayland.cpp` / `screencapturer/CMakeLists.txt` — 采集优先链接线、Wayland 采集器上报几何
- `src/remotedesk/server/inputmanger/inputmanager.h` / `inputmanager_linux.cpp` — 注入器接管与 uinput 强制模式
- `src/remotedesk/server/rdpserver/rdpserver.cpp` / `rdpserver.h` — Linux 守卫下接线注入器，代理接管时启用 uinput 语义
- `src/remotedesk/server/service/linux_service.cpp` — 3s tick 内切换采集源/拆装虚拟显示；生成并启用 user unit
- `src/remotedesk/server/service/service_linux.cpp` / `service/CMakeLists.txt` — 服务模式接入、链接
- `README.md` — 仅新增「Linux Wayland：systemd 服务 + 用户会话代理」一节（中英文各一处），其余段落未动

> 说明：以上按 Wayland 双进程改动整理；Android 的 FFmpeg/MediaCodec 硬编在同仓另有改动，未列入本清单。

关键修复（本轮）

1. **root 服务在代理退出时 SEGV**：`SessionAgentClient::onReadyRead()` 在 `handleMessage()` → `resetLink()` 置空 `socket_` 后继续解引用成员。改为缓存本地 `sock` 指针并用 `socket_ == sock` 守卫循环条件。
2. **注销重登后代理不再启动**：user unit 原来 `WantedBy=default.target`。注销时 `graphical-session.target` 被停（`PartOf` 连带停掉代理），但 `default.target` 在 user manager 生命周期内始终 active，重新登录不会再 start。改为 `WantedBy=graphical-session.target`，并在 install 时清理旧的 `default.target.wants` 链接。
3. **崩溃自愈上限**：unit 增加 `StartLimitIntervalSec=0`（`Restart=on-failure` + `RestartSec=3` 保留），避免默认 5 次/10 秒上限把持续异常的代理彻底停住。

portal 补验修复（v1.5.1）

4. **`CreateSession` 缺 `session_handle_token`**：portal 直接返回 `"Missing token"`，会话建不起来；补上该选项后恢复。
5. **把 Request 句柄当结果、未等 `Response` 信号**：`CreateSession` / `SelectSources` / `Start` 的返回值只是 Request 对象路径，真正结果（`session_handle` / `streams` / 用户授权码）通过 `org.freedesktop.portal.Request::Response` 信号返回。改为按 `handle_token` 预测请求路径、先预连接再同步等待 Response。
6. **`OpenPipeWireRemote` 参数类型错误**：第二参数签名是 `a{sv} options`，原代码传 `QString()`；改为 `QVariantMap()`。
7. **portal 的 streams 不带 size 导致代理误退出**：真实尺寸要等 PipeWire 格式协商完成才写入，`session_agent_run.cpp` 增加最多 2 s 的几何等待（仍拿不到才按原逻辑退出回落）。
8. **验证开关 `QTRD_FORCE_PORTAL`**：置 1 跳过 Mutter 分支，用于在 GNOME 机器上验证 portal 通用路径；**默认关闭**，常态启动顺序与行为不变。

## 3. 实测结论（192.168.1.53，2026-11-07）

| 项 | 结论 | 证据 |
|---|---|---|
| 真实桌面 vs Xvnc | 代理接管后采到的是 800×600 GNOME 真实桌面；回落才是 1920×1080 xfce 虚拟桌面 | 日志 `AgentCapturer: using session agent frames (800 x 600)` / `Using session-agent capture (real user desktop)`；画面 `relogin_10.png`（GNOME 顶栏、Anolis 壁纸）vs `stopcheck2_10.png`（xfce 面板、浅蓝壁纸） |
| 点选落点 | xev 探针 4 个点位误差 ≤1px，例：(410,320)→(409,319)、(520,390)→(519,389)；新会话内注入 (67,213) 后指针绘制在同一位置且图标被选中 | xev 回报 + 截图像素 |
| 键盘 | `code=KeyA/KeyZ/Space/Enter` 端到端可用；终端输入 `echo agentclickok` 有回显与输出 | `typed2_10.png` |
| 代理 stop/start | stop → 回落 X11/Xvnc（root 无崩溃）；start → 1s 内重新接管真实桌面 | 日志 `agent capture lost … fall back` → `Using session-agent capture` |
| 注销重登 | `systemctl restart lightdm` 后新会话里代理 2s 内自动启动（PID 42417），root 随即 attach；帧由 1920×1080 切到 800×600 | journal + `relogin_0/5/10.png` |
| kill -9 自愈 | kill 代理后 3s 由 systemd 拉起新 PID，root 无 SEGV，帧保持 800×600 | PID 42417 → 43125 |
| 授权被拒不黑屏 | 代理不进入采集时不触发虚拟屏 teardown，服务继续出回落画面 | 回落画面正常 |
| X11 未回归 | 243 机不可达，在 53 的 X11 回落上验证：click (410,328)→(410,328) 0px，a/z 键正确 | `DISPLAY=:99 xev` |
| Windows 未回归 | 64 位增量构建通过；Win7 32 位 10,168,345 B（sha256 19CAF182…）；XP 32 位 10,378,096 B（sha256 005ADF5E…） | 构建日志与标记校验 |

portal 路径补验（同一台机器，`QTRD_FORCE_PORTAL=1`，2026-11-07）

| 项 | 结论 | 证据 |
|---|---|---|
| 真的进 portal 分支 | 跳过 Mutter 后进入同一 `setupPortalScreenCast()` 并成功出帧 | journal：`QTRD_FORCE_PORTAL=1, skipping Mutter ScreenCast` → `portal session created` → `SelectSources ok` → `Start authorized, result keys= ("streams")` → `createStream ok, target node 36` → `negotiated format 800 x 600` |
| 首次授权 | 弹窗选显示器→点 Share 后拿到 800×600 真实 GNOME 桌面（非黑屏、非 Xvnc） | 弹窗 `portal2.png` / 选中 `p3.png`；帧 `portal_ws_10.png`（Home/Trash 图标、顶栏、Dock） |
| 坐标映射 | 基准 800×600 与帧一致；注入 (350,270)/(200,200)/(500,350)/(160,155) → xev root (349,269)/(199,199)/(499,349)/(159,154)，全部 **(-1,-1) px**（uinput 绝对轴取整）；点 Home 图标 (65,70) 可选中 | `DISPLAY=:0 xev -name XEvProbe` + WS 注入；`icon_click_11.png` |
| 取消授权 | `code=1`、代理退出、root 明确回落、画面不黑；再次启动代理会重新弹窗 | `portal Start rejected, code= 1` + `root service will keep using its local fallback`；帧 `cancel_fallback_10.png`（1920×1080 xfce）；`retry.png` |
| restore_token 复用 | **FAIL（后端限制）**：Start 结果只有 `streams`，无 `restore_token`，token 文件未生成，"第二次免弹窗"无法达成 | `~/.local/state/remotedesk/` 为空（目录 0700）；`xdg-desktop-portal-gnome 41.2` 二进制中无 `restore_token` / `persist_mode` 字符串 |
| 非 GNOME 合成器本体 | **未验证**：仓库中无 sway / weston / xdg-desktop-portal-wlr | `dnf list --available` 三者均 `No matching Packages to list` |

## 4. 已知限制

1. **手工停止代理会有约 10 秒过渡黑屏**：代理采集期间虚拟屏已被拆掉（不常驻空闲 Xvnc），手工 stop 会重建 Xvnc + 桌面会话，重建完成前是黑帧。异常崩溃（kill -9）几秒内重新接管，授权被拒根本不拆虚拟屏，这两种情况都不会黑屏。已决定不为此常驻虚拟屏。
2. **非 GNOME 合成器走 portal**：首次（以及 GNOME 41.2 这类不支持持久化的后端上每次启动）都需要用户在共享选择窗口确认。拒绝后代理以 0 退出（退出码 0 不触发 `Restart=on-failure`），服务保持回落画面；重试只发生在下次代理启动——重新登录或手工 `systemctl --user start remotedesk-agent`，**新的客户端 attach 不会自动重试**。portal 路径只在 GNOME 后端实测过，wlr/KDE 未验证。
3. **代理 hello 的初始几何值**：`WaylandCapturer` 初次上报可能是 1920×1080，首帧到达后立即校正为真实 800×600，并同步更新输入映射；无需人工干预，但连接最初 1 帧内不要依赖该值。
4. **uinput 只能按物理键码注入**：非 ASCII 文本无法写入 Wayland 合成器，代理会告警一次并丢弃（不静默失败）。
5. **依赖 systemd user manager 与 `graphical-session.target`**：非 systemd 管理的图形会话（如手工 `startx`）不会自动启动代理，需要手工运行 `QtRemoteDesktop --session-agent`。
6. **手工 stop 后不 start**：root 会一直停留在 Xvnc 回落，直到代理再次启动。
7. portal 授权 token 路径：`$XDG_STATE_HOME/remotedesk/portal_token`（0600，默认 `~/.local/state/remotedesk/portal_token`），所在目录 0700。只有后端确实返回 `restore_token` 时才写文件；GNOME 41.2（`xdg-desktop-portal-gnome`）未实现持久授权，本机该目录为空、每次启动都会重新弹窗。

## 5. 运维命令

```bash
# 状态
systemctl status remotedesk
systemctl --user status remotedesk-agent

# 重启（先 root 再代理）
sudo systemctl restart remotedesk
systemctl --user restart remotedesk-agent

# 临时退回旧的单进程形态
sudo systemctl edit remotedesk   # Environment=QTRD_DISABLE_SESSION_AGENT=1
```

## 6. 提交状态与未做的动作

- Wayland 双进程实现与文档随 v1.5.0 提交；portal 路径的四项修复与本文件/README 的校正随 v1.5.1 提交推送。
- 留作后续可选改进（本轮按约定不改）：授权被拒后不自动重试（新客户端 attach 不拉起代理）；`QTRD_FORCE_PORTAL` 保持默认关闭，仅作验证开关。
- Android 的 FFmpeg/MediaCodec 硬编改动（`src/remotedesk/android/`、`android_build_script`、`videoencoder`、`screencapturer.h`、`src/CMakeLists.txt`、`FindFFmpeg.cmake` 等）不在本说明范围内，也未改动。
- 测试机已还原：`QTRD_FORCE_PORTAL` 已从 user manager 环境移除，代理回到 Mutter 路径并确认 800×600 真实桌面；临时注入器/截图/xev 已清理；`/etc/lightdm/lightdm.conf` 内容与软件包一致（自动登录关闭，`rpm -V` 仅 mtime 不同），未安装任何合成器包。
