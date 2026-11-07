# Linux Wayland 会话代理：终态说明

> 适用范围：Linux + Wayland（GNOME/mutter 实测）上的「root 系统服务 + 用户会话代理」双进程形态。
> 状态：实现完成并在 192.168.1.53（Anolis 8 / GNOME Wayland / 800×600）实测通过；未提交、未推送。

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

## 4. 已知限制

1. **手工停止代理会有约 10 秒过渡黑屏**：代理采集期间虚拟屏已被拆掉（不常驻空闲 Xvnc），手工 stop 会重建 Xvnc + 桌面会话，重建完成前是黑帧。异常崩溃（kill -9）几秒内重新接管，授权被拒根本不拆虚拟屏，这两种情况都不会黑屏。已决定不为此常驻虚拟屏。
2. **非 GNOME 合成器走 portal**：首次需要用户在共享选择窗口确认；拒绝后代理以 0 退出（不进入重启循环、不重复弹窗），服务保持回落画面。
3. **代理 hello 的初始几何值**：`WaylandCapturer` 初次上报可能是 1920×1080，首帧到达后立即校正为真实 800×600，并同步更新输入映射；无需人工干预，但连接最初 1 帧内不要依赖该值。
4. **uinput 只能按物理键码注入**：非 ASCII 文本无法写入 Wayland 合成器，代理会告警一次并丢弃（不静默失败）。
5. **依赖 systemd user manager 与 `graphical-session.target`**：非 systemd 管理的图形会话（如手工 `startx`）不会自动启动代理，需要手工运行 `QtRemoteDesktop --session-agent`。
6. **手工 stop 后不 start**：root 会一直停留在 Xvnc 回落，直到代理再次启动。
7. portal 授权 token 存在用户 state 目录：`$XDG_STATE_HOME/remotedesk/portal_token`（0600，默认 `~/.local/state/remotedesk/portal_token`）。

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

## 6. 未做的动作

- 未 `git commit` / `git push`。
- Android 的 FFmpeg/MediaCodec 硬编改动（`src/remotedesk/android/`、`android_build_script`、`videoencoder`、`screencapturer.h`、`src/CMakeLists.txt`、`FindFFmpeg.cmake` 等）不在本说明范围内，也未改动。
- 测试机已还原 `/etc/lightdm/lightdm.conf`（自动登录关闭）。
