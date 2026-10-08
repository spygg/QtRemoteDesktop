# QtRemoteDesktop

**Web-based, cross-platform remote desktop — control any PC from your browser, built in C++/Qt for high performance.**

QtRemoteDesktop is a self-hosted, open-source remote desktop solution. The **client runs entirely in the browser** (no app to install on the controlling side), while the **host is a native C++/Qt program** that works on **Windows (XP → 11), Linux (X11 & Wayland), ARM boards (RK3588), macOS and Android** — with support for **lock-screen detection & remote unlock**, **multi-monitor switching**, **two-way clipboard sync (text & image)**, **bidirectional file transfer (drag & drop)**, and **three low-latency video pipelines (Image / WebRTC / FFmpeg-MSE)**.

> 🇨🇳 中文版见 [下半部分](#中文版chinese)

[![GitHub stars](https://img.shields.io/github/stars/spygg/QtRemoteDesktop)](https://github.com/spygg/QtRemoteDesktop) [![GitHub forks](https://img.shields.io/github/forks/spygg/QtRemoteDesktop)](https://github.com/spygg/QtRemoteDesktop) [![Language: C++](https://img.shields.io/badge/language-C%2B%2B%2FQt-blue)](https://github.com/spygg/QtRemoteDesktop) [![Platform: Windows/Linux/macOS/Android](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android-lightgrey)](https://github.com/spygg/QtRemoteDesktop) [![Client: Browser](https://img.shields.io/badge/client-Browser%20(no%20install)-green)](https://github.com/spygg/QtRemoteDesktop) [![Transport: WebRTC/MSE/JPEG](https://img.shields.io/badge/transport-WebRTC%20%7C%20MSE%20%7C%20JPEG-orange)](https://github.com/spygg/QtRemoteDesktop)

---

## ✨ Highlights

### 1. 🖥 Browser-based client — zero install
- Open `http://<host>:8080` in **Chrome / Edge / Firefox / Safari** — no client software, no plugins, no account registration.
- Works from **Windows, macOS, Linux, Android tablets and even phones** as the *controlling* side.
- Three video modes with automatic fallback:
  - **WebRTC** — lowest latency, browser-native RTP decoding (H.264).
  - **FFmpeg / MSE** — H.264/HEVC over WebSocket + MediaSource (works without WebCodecs).
  - **Image (JPEG)** — maximum compatibility, frame-by-frame.

### 2. 🧩 Cross-platform — host runs almost everywhere
| Platform | Status |
|---|---|
| **Windows XP / 7 / 8 / 10 / 11** | ✅ Full support (DXGI/D3D11 capture + GDI fallback, Windows service mode) |
| **Linux X11** | ✅ Full support (XDamage/XComposite capture, XTest input) |
| **Linux Wayland** | ✅ Input via uinput / XDG Desktop Portal / XTest (auto channel selection); capture via PipeWire |
| **Linux ARM (e.g. RK3588)** | ✅ Verified in production; Rockchip MPP hardware H.264 encoding |
| **macOS** | 🧪 Experimental skeleton (screen capture & input implemented, not yet verified) |
| **Android** | 🧪 Experimental skeleton |
| **Browser as controller** | ✅ Any modern browser |

### 3. 🔒 Lock screen & system control
- **Lock-screen detection** on the host (Windows desktop switch / XP screen saver / Linux).
- **Remote unlock** — type the password from the browser, or one-click "Unlock" that switches back to the desktop VT without a password.
- System menu: **Lock / Unlock, Show Desktop, Task Manager, Logout, Reboot, Shutdown** — with confirmation prompts.
- Remote **IME (input method) switching** for Linux hosts.

### 4. ⚡ C++ / Qt high performance
- Native **C++/Qt 5/6** host — no Electron, no Java; low CPU/memory footprint.
- **Hardware video encoding**: Rockchip **MPP** (RK3588) + FFmpeg/openh264 software fallback; NVIDIA/AMD not required.
- **Hardware video decoding** in the browser via **WebCodecs** (`prefer-hardware`), falling back to MSE / native WebRTC decode.
- **Low-latency rendering**: latest-frame-only JPEG pipeline, `createImageBitmap` GPU scaling, 60 Hz input rate.
- **Back-pressure control** (8 MB per-client media backlog), keyframe-based GOP resync, heartbeat keep-alive, auto-reconnect.

---

## 📋 Feature list

- **Screen capture**: multi-monitor enumeration & switching, resolution change from the browser, DXGI→GDI fallback (Windows), XDamage incremental (X11), PipeWire (Wayland).
- **Video pipelines**: Image (JPEG) / WebRTC (RTP, browser-native decode) / FFmpeg-MSE (H.264/HEVC via WebSocket).
- **Quality / FPS / Scale**: hot-switchable in *all* video modes (high/medium/low/verylow ↔ bitrate & JPEG quality; 60/30/15 fps; 100–25 % scale).
- **Input**: mouse, keyboard, wheel, multi-button; Unicode char injection; Win/meta key passthrough; IME composition safe.
- **Clipboard sync (two-way)**: text + image, with a smart arbitration engine that decides between "local clipboard" and "remote clipboard" using copy/cut/blur timestamps — **pasting works correctly even when the local clipboard already holds old content**.
- **File transfer (two-way)**:
  - Browse remote file system in the browser (real absolute paths, restricted root).
  - Download files / folders (streaming TAR for directories, HTTP Range support, chunked WS fallback).
  - Upload with chunked 64 KB frames + back-pressure + integrity check.
  - **Drag & drop both ways**: drop files from your computer into the remote desktop (respects the drop target folder), or drag remote files out to download.
- **Lock screen**: detection, in-browser password unlock, one-click VT unlock, XP screen-saver handling.
- **Security**: username/password + token session auth, optional **HTTPS/WSS** (self-signed or your own cert), loopback-only internal channels, per-route auth checks.
- **Remote shell** (Linux/macOS true PTY).
- **Service mode**: Windows service + helper process in user session; Linux systemd service; sleep inhibitor during active sessions.
- **Stability**: crash dump via Breakpad, daily log files with level filtering, single-instance guard.

---

## 🚀 Quick start

```bash
# Build (Linux)
./src/linux_build_script/build_linux.sh          # zero-config, auto-detects Qt
# Build (Windows)
src\windows_build_script\build.bat

# Run
./bin/QtRemoteDesktop                              # Linux
bin\QtRemoteDesktop.exe                            # Windows
```

1. Open your browser: `http://<host-ip>:8080`
2. Log in with the default account `admin` / `admin`
3. Choose video mode in the toolbar: **Auto / WebRTC** (lowest latency) → **FFmpeg** → **Image** (max compatibility)
4. Done — you now control the remote desktop from the browser. 🎉

> Default ports: HTTP `8080`, WebSocket `8081` (= HTTP port + 1). All options: `--no-ssl`, `--log-level debug|info|warning|critical`, `--install/--uninstall/--service`, `--helper`.

---

## 🔧 Build requirements

| Platform | Requirements |
|---|---|
| **Windows** | Qt 5.x (MinGW or MSVC, needs **QtWebSockets**), CMake ≥ 3.15, Git Bash, Windows SDK |
| **Linux** | gcc/g++/make, CMake, Qt5 base + WebSockets, X11/Xtst/Xdamage/Xcomposite/Xrender/Xfixes dev packages, OpenSSL, zlib; optional PipeWire (Wayland), libXrandr (multi-monitor), libdrm (RK3588 MPP) |
| **All** | Third-party libs (metaRTC / srtp / usrsctp / openh264 / FFmpeg / MPP) are **built from source** automatically (`BUILD_THIRDPARTY=ON`); CMake options `USE_FFMPEG`, `USE_WEBRTC` |

Debian/Ubuntu one-liner:

```bash
sudo apt-get install -y build-essential cmake pkg-config \
    qtbase5-dev qtwebsockets5-dev \
    libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev libxfixes-dev \
    libssl-dev zlib1g-dev libasound2-dev libpcap-dev \
    libpipewire-0.3-dev mlocate
```

RHEL / Fedora / Anolis:

```bash
sudo dnf install -y gcc gcc-c++ make cmake pkgconfig \
    qt5-qtbase-devel qt5-qtwebsockets-devel \
    libX11-devel libXtst-devel libXdamage-devel libXcomposite-devel libXrender-devel libXfixes-devel \
    openssl-devel zlib-devel alsa-lib-devel libpcap-devel \
    pipewire-devel mlocate
```

Dependency self-check: `./src/linux_build_script/build_linux.sh --check-deps`

---

## 📖 Usage tips

- **Video modes**: Auto tries WebRTC first (lowest latency), falls back through FFmpeg/MSE to Image automatically.
- **Quality / FPS / Scale** apply immediately in every mode — no reconnect needed.
- **Multi-monitor**: switch capture target from the toolbar (screen menu) when the host has several displays.
- **Resolution**: pick a resolution from the menu; the host switches its display and the stream follows.
- **Clipboard**: copy on either side, paste on the other — text and images both work. The arbitration engine automatically prefers "what the user just copied".
- **Files**: use the file panel to browse/download/upload, or simply **drag & drop** between your computer and the remote desktop.
- **System menu**: Lock / Unlock screen, Show Desktop, Task Manager, Logout, Reboot, Shutdown.
- **Locked host?** The page shows a lock screen — type the host password, or click **Unlock** for the one-click desktop unlock.

---

## ❓ FAQ

**Q: Do I need to install anything on the controlling computer?**
A: No. Any modern browser works. Only the *host* (controlled) machine runs the C++ program.

**Q: Does it need internet / cloud servers?**
A: No. It is fully self-hosted on your LAN. Works offline, no relay, no account registration, no data leaves your network. (Public access is possible by exposing the HTTP/WS port.)

**Q: Which browsers are supported?**
A: Chrome/Edge (best: WebRTC + WebCodecs + MSE), Firefox (WebRTC/MSE), Safari (WebRTC/MSE). The Image mode works on essentially everything.

**Q: How about latency?**
A: On a LAN, WebRTC mode provides near-real-time control. Design choices (latest-frame-only JPEG, keyframe-guided GOP resync, back-pressure) keep latency bounded under load.

**Q: Does it work on Windows XP?**
A: Yes — the Windows build keeps XP compatibility (no `AcquireSRWLockExclusive`/`inet_ntop` dependency issues, WTS API used dynamically).

**Q: Why can't I see the screen when the host is locked?**
A: You still can — the browser shows a lock UI and lets you type the password or one-click unlock. On some Linux setups (light-locker without greeter) the capture goes black; kill `light-locker` or use the Unlock action.

---

## 🏗 Tech stack

- **C++/Qt 5/6** (Core / Gui / Widgets / Network / WebSockets), **CMake**
- **FFmpeg + openh264** software H.264; **Rockchip MPP** hardware H.264 (RK3588)
- **metaRTC** (WebRTC: ICE / DTLS / SRTP / RTP)
- **DXGI / D3D11 / GDI** (Windows capture), **X11 + PipeWire** (Linux capture), **uinput / XDG Desktop Portal / XTest** (Linux input)
- **WebCodecs / MSE / WebSocket** (browser-side decode & transport)

---

## 🤝 Contributing

Issues and Pull Requests are welcome. Please open an issue first for major changes.

## 📄 License

See the `LICENSE` file in the repository.

---
---

# 中文版（Chinese）

# QtRemoteDesktop

**基于 Web 的跨平台远程桌面 —— 浏览器即可控制任意电脑，C++/Qt 原生高性能。**

QtRemoteDesktop 是一款可自托管的开源远程桌面软件：**控制端完全运行在浏览器里**（无需安装任何客户端），**被控端是原生 C++/Qt 程序**，支持 **Windows（XP → 11）、Linux（X11 与 Wayland）、ARM 开发板（RK3588）、macOS 与 Android**，并内置 **锁屏检测与远程解锁**、**多屏切换**、**剪贴板双向同步（文本+图片）**、**文件双向传输（支持拖拽）**，以及三条低延迟视频通道（**图片 / WebRTC / FFmpeg-MSE**）。

---

## ✨ 项目亮点

### 1. 🖥 浏览器即客户端，零安装
- 用 **Chrome / Edge / Firefox / Safari** 打开 `http://<被控机>:8080` 即可控制，无需安装客户端、无需插件、无需注册账号。
- 控制端可以是 Windows、macOS、Linux、平板甚至手机。
- 三种视频模式自动降级：
  - **WebRTC** —— 延迟最低，浏览器原生 RTP 解码（H.264）。
  - **FFmpeg / MSE** —— WS 推送 H.264/HEVC + MediaSource 播放（不依赖 WebCodecs）。
  - **图片（JPEG）** —— 兼容性最好，逐帧显示。

### 2. 🧩 跨平台 —— 被控端几乎通吃
| 平台 | 状态 |
|---|---|
| **Windows XP / 7 / 8 / 10 / 11** | ✅ 完整支持（DXGI/D3D11 捕获 + GDI 回退、Windows 服务模式） |
| **Linux X11** | ✅ 完整支持（XDamage/XComposite 捕获、XTest 输入） |
| **Linux Wayland** | ✅ 输入三通道自动切换（uinput / XDG Desktop Portal / XTest）；PipeWire 捕获 |
| **Linux ARM（如 RK3588）** | ✅ 已实际部署验证；Rockchip MPP 硬件 H.264 编码 |
| **macOS** | 🧪 实验骨架（捕获与输入已实现，未实测） |
| **Android** | 🧪 实验骨架 |
| **浏览器作为控制端** | ✅ 任意现代浏览器 |

### 3. 🔒 锁屏与系统控制
- 被控端**锁屏自动检测**（Windows 桌面切换 / XP 屏保 / Linux）。
- **远程解锁** —— 浏览器里直接输入密码，或一键「解锁屏幕」免密切回桌面 VT。
- 系统菜单：**锁屏 / 解锁、显示桌面、任务管理器、注销、重启、关机**（危险操作带确认）。
- 远端 **输入法切换**（Linux ibus）。

### 4. ⚡ C++/Qt 高性能
- 被控端为原生 **C++/Qt 5/6**，无 Electron、无 JVM，CPU/内存占用低。
- **硬件视频编码**：Rockchip **MPP**（RK3588）+ FFmpeg/openh264 软编回退，无需独立显卡。
- 浏览器端 **WebCodecs 硬解**（`prefer-hardware`），并回退 MSE / WebRTC 原生解码。
- **低延迟渲染**：图片模式"最新帧优先"单飞解码、`createImageBitmap` GPU 缩放、60Hz 输入。
- **背压控制**（每客户端 8MB 媒体积压上限）、关键帧引导 GOP 重同步、心跳保活、自动重连。

---

## 📋 功能列表

- **屏幕捕获**：多显示器枚举与切换、浏览器端改分辨率、DXGI→GDI 回退（Windows）、XDamage 增量（X11）、PipeWire（Wayland）。
- **视频通道**：图片（JPEG）/ WebRTC（RTP 原生解码）/ FFmpeg-MSE（H.264/HEVC over WS）。
- **画质 / 帧率 / 缩放**：所有视频模式下**即时生效**（高/中/低/极低 ↔ 码率与 JPEG 质量；60/30/15 帧；100–25% 缩放）。
- **输入**：鼠标、键盘、滚轮、多键；Unicode 字符注入；Win/meta 键透传；输入法合成安全。
- **剪贴板双向（文本+图片）**：内置仲裁引擎，基于 copy/cut/blur 时间戳判断用"本机剪贴板"还是"远端剪贴板"——**被粘贴机器剪贴板已有旧内容时也能正确粘贴**。
- **文件双向传输**：
  - 浏览器浏览远端文件系统（真实绝对路径、受限根目录）。
  - 下载文件/目录（目录流式 TAR、HTTP Range、WS 分块兜底）。
  - 上传（64KB 分块 + 背压 + 完整性校验）。
  - **双向拖拽**：本机文件拖入远端（按落点目录落盘），远端文件拖出即下载。
- **锁屏**：检测、浏览器内输入密码解锁、一键 VT 解锁、XP 屏保处理。
- **安全**：用户名/密码 + token 会话认证、可选 **HTTPS/WSS**（自签或自有证书）、内部通道仅限回环、逐路由鉴权。
- **远程 Shell**（Linux/macOS 真 PTY）。
- **服务模式**：Windows 服务 + 用户会话 helper 进程；Linux systemd 服务；远程会话期间抑制睡眠。
- **稳定性**：Breakpad 崩溃转储、按日日志 + 分级过滤、单实例保护。

---

## 🚀 快速开始

```bash
# 构建（Linux）
./src/linux_build_script/build_linux.sh          # 零参数自动探测 Qt
# 构建（Windows）
src\windows_build_script\build.bat

# 运行
./bin/QtRemoteDesktop                              # Linux
bin\QtRemoteDesktop.exe                            # Windows
```

1. 浏览器打开 `http://<被控机IP>:8080`
2. 默认账号 `admin` / `admin` 登录
3. 顶部工具栏切换视频模式：**自动 / WebRTC**（最低延迟）→ **FFmpeg** → **图片**（最高兼容）
4. 完成，浏览器即开始控制远端桌面。🎉

> 默认端口：HTTP `8080`，WebSocket `8081`（= HTTP 端口 + 1）。常用参数：`--no-ssl`、`--log-level debug|info|warning|critical`、`--install/--uninstall/--service`、`--helper`。

---

## 🔧 构建依赖

| 平台 | 依赖 |
|---|---|
| **Windows** | Qt 5.x（MinGW 或 MSVC，需含 **QtWebSockets**）、CMake ≥ 3.15、Git Bash、Windows SDK |
| **Linux** | gcc/g++/make、CMake、Qt5 base + WebSockets、X11/Xtst/Xdamage/Xcomposite/Xrender/Xfixes 开发包、OpenSSL、zlib；可选 PipeWire（Wayland）、libXrandr（多屏）、libdrm（RK3588 MPP） |
| **全部** | 第三方库（metaRTC / srtp / usrsctp / openh264 / FFmpeg / MPP）**自动从源码构建**（`BUILD_THIRDPARTY=ON`）；CMake 选项 `USE_FFMPEG`、`USE_WEBRTC` |

Debian/Ubuntu 一键安装：

```bash
sudo apt-get install -y build-essential cmake pkg-config \
    qtbase5-dev qtwebsockets5-dev \
    libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev libxfixes-dev \
    libssl-dev zlib1g-dev libasound2-dev libpcap-dev \
    libpipewire-0.3-dev mlocate
```

RHEL / Fedora / 龙蜥 Anolis：

```bash
sudo dnf install -y gcc gcc-c++ make cmake pkgconfig \
    qt5-qtbase-devel qt5-qtwebsockets-devel \
    libX11-devel libXtst-devel libXdamage-devel libXcomposite-devel libXrender-devel libXfixes-devel \
    openssl-devel zlib-devel alsa-lib-devel libpcap-devel \
    pipewire-devel mlocate
```

依赖自检：`./src/linux_build_script/build_linux.sh --check-deps`

---

## 📖 使用说明

- **视频模式**：Auto 优先尝试 WebRTC（最低延迟），失败自动经 FFmpeg/MSE 降级到图片。
- **画质 / 帧率 / 缩放**：所有模式即时生效，无需重连。
- **多屏切换**：被控端有多显示器时，工具栏「屏幕」菜单选择捕获目标。
- **分辨率**：菜单选择分辨率，被控端自动切换显示并跟随推流。
- **剪贴板**：任一端复制、另一端粘贴，文本与图片均支持；仲裁引擎自动优先"用户刚复制的内容"。
- **文件**：文件面板浏览/下载/上传，或直接**双向拖拽**。
- **系统菜单**：锁屏 / 解锁、显示桌面、任务管理器、注销、重启、关机。
- **被控端锁屏了？** 页面显示锁屏界面——输入被控端密码，或点「解锁屏幕」一键免密切回桌面。

---

## ❓ 常见问题

**Q: 控制端需要安装什么吗？**
A: 什么都不用装，任意现代浏览器即可。只有被控端运行 C++ 程序。

**Q: 需要联网 / 云服务器吗？**
A: 不需要。完全自托管于局域网，离线可用，无中继、无注册、数据不出内网。（如需公网访问，自行暴露 HTTP/WS 端口即可。）

**Q: 支持哪些浏览器？**
A: Chrome/Edge（最佳：WebRTC + WebCodecs + MSE）、Firefox（WebRTC/MSE）、Safari（WebRTC/MSE）。图片模式几乎全兼容。

**Q: 延迟怎么样？**
A: 局域网内 WebRTC 模式接近实时。设计上（图片模式最新帧优先、关键帧引导 GOP 重同步、背压控制）保证高负载下延迟有界。

**Q: 支持 Windows XP 吗？**
A: 支持——Windows 构建保持 XP 兼容（无 `AcquireSRWLockExclusive`/`inet_ntop` 依赖问题，WTS API 动态加载）。

**Q: 被控端锁屏后为什么黑屏？**
A: 锁屏后仍可操作：浏览器显示锁屏 UI，可输入密码或一键解锁。部分 Linux 环境（light-locker 无 greeter）捕获会黑屏——`pkill light-locker` 或使用「解锁屏幕」即可。

---

## 🏗 技术栈

- **C++/Qt 5/6**（Core / Gui / Widgets / Network / WebSockets）、**CMake**
- **FFmpeg + openh264** 软编 H.264；**Rockchip MPP** 硬编 H.264（RK3588）
- **metaRTC**（WebRTC：ICE / DTLS / SRTP / RTP）
- **DXGI / D3D11 / GDI**（Windows 捕获）、**X11 + PipeWire**（Linux 捕获）、**uinput / XDG Desktop Portal / XTest**（Linux 输入）
- **WebCodecs / MSE / WebSocket**（浏览器端解码与传输）

---

## 🤝 参与贡献

欢迎提交 Issue 与 Pull Request。重大改动请先开 Issue 讨论。

## 📄 许可证

请见仓库内 `LICENSE` 文件。
