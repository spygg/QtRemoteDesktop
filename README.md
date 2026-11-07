# QtRemoteDesktop

**Web-based, cross-platform remote desktop — control any PC from your browser, built in C++/Qt for high performance.**

QtRemoteDesktop is a self-hosted, open-source remote desktop solution. The **client runs entirely in the browser** (no app to install on the controlling side), while the **host is a native C++/Qt program** that works on **Windows (XP → 11), Linux (X11 & Wayland), ARM boards (RK3588), macOS and Android** — with support for **lock-screen detection & remote unlock**, **multi-monitor switching**, **two-way clipboard sync (text & image)**, **bidirectional file transfer (drag & drop)**, and **three low-latency video pipelines (Image / WebRTC / FFmpeg-MSE)**.

> 🇨🇳 中文版见 [下半部分](#中文版chinese)

[![License: MIT](https://img.shields.io/github/license/spygg/QtRemoteDesktop)](https://github.com/spygg/QtRemoteDesktop) [![GitHub stars](https://img.shields.io/github/stars/spygg/QtRemoteDesktop)](https://github.com/spygg/QtRemoteDesktop) [![GitHub forks](https://img.shields.io/github/forks/spygg/QtRemoteDesktop)](https://github.com/spygg/QtRemoteDesktop) [![Language: C++](https://img.shields.io/badge/language-C%2B%2B%2FQt-blue)](https://github.com/spygg/QtRemoteDesktop) [![Platform: Windows/Linux/macOS/Android](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android-lightgrey)](https://github.com/spygg/QtRemoteDesktop) [![Client: Browser](https://img.shields.io/badge/client-Browser%20(no%20install)-green)](https://github.com/spygg/QtRemoteDesktop) [![Transport: WebRTC/MSE/JPEG](https://img.shields.io/badge/transport-WebRTC%20%7C%20MSE%20%7C%20JPEG-orange)](https://github.com/spygg/QtRemoteDesktop)

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

### Linux Wayland: systemd service + user-session agent

On Wayland only a process inside the logged-in user session can capture the real desktop; a root-only service captures the virtual X display (Xvnc) instead. `--install` therefore sets up two cooperating processes:

- `remotedesk.service` (system, root): ports, config, encoding, lock state, virtual-display fallback.
- `remotedesk-agent.service` (user unit, desktop user, started at login): PipeWire ScreenCast capture and input injection; talks to the service over `$XDG_RUNTIME_DIR/remotedesk/session-agent.sock` (mode 0600), with frames in `/dev/shm/remotedesk-agent.<uid>.<pid>`.

Notes:

- Capture order: session agent → PipeWire (Wayland) → X11 → Xvnc. If the agent stops or its authorization is denied, the service keeps serving the fallback source instead of a black screen.
- Fallback transition: while the agent is capturing, the local virtual display is torn down (no idle Xvnc session is kept around). A **manual** `systemctl --user stop remotedesk-agent` therefore rebuilds Xvnc + the desktop session, which shows a short black frame (about 10 s) until the fallback desktop comes up. A crash (`kill -9`) re-attaches within a few seconds and a denied portal grant never tears the display down, so neither of those leaves a black screen.
- Input prefers the user session's `/dev/uinput`. When it is not writable, the install/startup output prints `usermod -aG input <user>` (effective at the next login; the installer never changes permissions itself) and input falls back to the root-side injector.
- The size reported by the agent is the sole coordinate basis; the root side applies no extra scaling.
- The portal grant is persisted in `$XDG_STATE_HOME/remotedesk/portal_token` (0600, default `~/.local/state/remotedesk/portal_token`) when the portal supports `restore_token` (Mutter ScreenCast needs no dialog).
- Kill switch: set `QTRD_DISABLE_SESSION_AGENT=1` for `remotedesk.service` to get the old single-process behavior.
- Restart: `systemctl restart remotedesk && systemctl --user restart remotedesk-agent`; a crashed agent is restarted automatically after 3 s.

### Windows 32-bit: one command, two packages (Win7 6.1.2 + XP 3.4.8)

```bat
src\windows_build_script\build_32pair.bat          :: both, Win7 first
src\windows_build_script\build_32pair.bat win7     :: FFmpeg 6.1.2 only
src\windows_build_script\build_32pair.bat xp       :: FFmpeg 3.4.8 only
```

Produces `bin_win7_612\QtRemoteDesktop.exe` (FFmpeg 6.1.2) and `bin_xp_348\QtRemoteDesktop.exe` (FFmpeg 3.4.8), then prints **size + SHA256** of both exes.

Rules the script enforces (also documented in `build_onekey_build*.ps1`, `build.bat` and `build_32pair.bat`):

1. **Build order: Win7 (6.1.2) first, XP (3.4.8) second.**
2. **Delete `build_output_win32\lib\*.a` before every build.** Both 32-bit trees write their static libs to that one shared directory (`CMAKE_ARCHIVE_OUTPUT_DIRECTORY`), and `libVideoEncoder.a` embeds FFmpeg-version-specific objects. An archive left by the other tree is linked as-is — the 6.1.2 link then fails with `undefined reference to 'avcodec_register_all'`, while the 3.4.8 link silently produces an exe **without FFmpeg** (`7,394,065` bytes instead of ~`10.2 MB`, observed 2026-11-04).
3. **Verify before shipping:** each exe must contain its own FFmpeg marker (`av_channel_layout_default` for 6.1.2, `avcodec_register_all` for 3.4.8), must *not* contain the other one, and must contain openh264 (`WelsCreateSVCEncoder`). The pair script does this automatically.
4. Compare the printed SHA256 with the exe actually deployed on the target machine.

### Android: one command, debug APK (Qt 6, arm64-v8a)

```bat
src\android_build_script\build_android.bat
```

The script uses the Qt 6 `android_arm64_v8a` kit via `qt-cmake` (`QT_HOST_PATH=mingw_64`) with
`ANDROID_ABI=arm64-v8a`, `USE_FFMPEG=ON`, `USE_WEBRTC=OFF`, `BUILD_THIRDPARTY=OFF`, builds the
`apk` target, and finally prints the **APK path, size, SHA256 and the `adb install` command**.

Before CMake is configured, `src\android_build_script\build_ffmpeg_android.ps1` cross-compiles
**FFmpeg 6.1.2 for arm64-v8a** into `build_output\ffmpeg_install-6.1.2-android-arm64`
(`--enable-mediacodec --enable-jni --enable-encoder=h264_mediacodec`, static, `-fPIC`, API 24,
no openh264). The directory name is version/arch namespaced, so the Android libs can never be
confused with the desktop MinGW/pe-i386 build; the step is signature-checked and skipped when the
artifacts are already up to date (`build_ffmpeg_android.ps1 -Force` to rebuild). The first build
takes roughly **6–12 minutes on 8 threads**; the APK grows by about **0.7 MB** (measured
24,914,585 → 25,658,891 bytes on the arm64-v8a debug build) because libavcodec/libavutil/libswscale
are linked in. H.264 frames are encoded by the phone's **MediaCodec**
hardware encoder (NV12 system-memory input, no JavaVM needed). If the device exposes no H.264
encoder, the server reports video as **unsupported** and the browser stays in JPEG mode — it never
claims a capability the build/device cannot deliver.

It is idempotent: an existing `build_android\` tree is reconfigured and rebuilt incrementally;
pass `-Clean` to start over. On failure it prints the **first real error**
(`FAILED:` / `CMake Error` / `error:`) and the full log path instead of a wall of output.

Prerequisites:

| Item | Requirement |
|---|---|
| Qt | Qt 6.x with the **android_arm64_v8a** kit **and** the matching **mingw_64** host tools (`bin\qt-cmake.bat`) |
| Android SDK | platform-tools, build-tools, one platform, `cmake` + `ninja` |
| NDK | r27 (`27.1.12297006` preferred; other versions can be forced with `-NdkRoot`) |
| JDK | 17+ with `JAVA_HOME` set (Gradle requirement) |
| Gradle | wrapper already points at the Tencent mirror; Maven Central can be accelerated with an Aliyun init script (`%USERPROFILE%\.gradle\init.gradle`) when needed |

Environment overrides: `QT_ANDROID_ROOT`, `ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT`;
script options: `-QtRoot`, `-BuildDir`, `-NdkRoot`, `-Clean`.

Install and start:

```bat
adb install -r build_android\remotedesk\android-build\build\outputs\apk\debug\android-build-debug.apk
adb shell am start -n com.radiometer.remotedesk/.MainActivity
```

Then on the phone tap **开始共享屏幕** → in the system dialog choose **整个屏幕** → **立即开始**,
and open `http://<phone-ip>:8080` on the PC browser (`admin` / `admin`).

**Three manual grants are required (no way around them):**

1. **Screen capture permission** — Android 14 asks again on every app-process restart (reboot,
   force-stop, reinstall). The in-app button starts the flow; the system dialog must be confirmed.
2. **Accessibility service** — `设置 → 无障碍 → RemoteDesk 输入服务` (one-time, survives restarts).
   Required for tap / swipe / long-press / back / home **and keyboard text input** (Android does not
   expose key injection to normal apps). If it is off, the browser shows an explicit `input_status`
   error and **no event is injected** (nothing is silently dropped).
3. **Modify system settings** (`WRITE_SETTINGS`) — only needed for **orientation switching while the
   RemoteDesk app is in the background** (the normal "phone on the side, drive everything from the PC"
   use). Without it, orientation only applies while the app window is in the foreground and the browser
   receives `write_settings_required` with instructions. Grant once:
   ```bash
   adb shell appops set com.radiometer.remotedesk android:write_settings allow
   ```
   or on the phone: `设置 → 应用 → RemoteDesk → 特殊权限 → 修改系统设置`. The Android-only
   **「授权横屏」** button in the browser toolbar opens that page directly (via
   `ACTION_MANAGE_WRITE_SETTINGS`); once granted the browser hides the button and shows a
   confirmation. Side effect: the system **auto-rotate** switch is turned off (the chosen orientation
   is kept); re-enable it in quick settings if you want sensor rotation back.
   Note: an app that forces its own orientation (launcher, lock screen, some media apps) overrides the
   system rotation lock, so a switch can time out while such an app is in front — the browser shows
   `orientation_timeout` with this reason. Open an app that follows system rotation first.

**Chinese / Unicode text input (desktop and mobile browsers):** while the browser's own IME is
composing, nothing is forwarded; on `compositionend` the whole committed string is sent once as an
`input_text` message and written into the focused field on the phone with the accessibility
`ACTION_SET_TEXT` action (no dependency on the phone's IME candidates). English, digits and
punctuation keep using the per-key path, and both paths share one shadow-text state, so
"测试" → "中文" → "abc123中文" typed in sequence merges in order without duplication or loss.
On a touch device the toolbar **「键盘」** button raises the local soft keyboard into a hidden
sink (tapping the remote screen alone does not pop the keyboard); its committed text travels the
same path. Limits: only the **currently focused editable control** receives text, protected fields
that reject `ACTION_SET_TEXT` return `keyboard_key_failed`, no focus returns `keyboard_no_focus`
(both shown in the browser); the phone's own candidate window is not visible/controllable from the
PC, and keyboard shortcuts / arrow keys are still unsupported.

File manager on Android starts at a virtual root containing `/sdcard` (shared storage) and the app's
private `files` directory; both list names and sizes and can be entered. Directories the app cannot read
(for example protected system paths) return an explicit error message instead of an empty panel.
Scoped storage may still hide some shared folders — that is reported as a permission error, not blank.

Supported: live view; click / swipe / long-press / back / home; **keyboard text into the currently focused
input** (English letters, digits, space, common punctuation, Enter, Backspace, and **Chinese / any
Unicode text committed by the browser's IME via `input_text`**); portrait & landscape
(the canvas and input mapping follow the real 720x1612 ↔ 1612x720, and a switch is only acknowledged
after the captured frame size actually changes); the 8080 web page stays reachable while the app is in
the background (foreground capture service).

Not supported yet: keyboard shortcuts (Ctrl/Alt combinations), arrow/function keys, the phone's own
IME candidate window (text is committed from the browser side instead), multi-touch (pinch), audio,
file transfer, web terminal, and unattended/headless operation. Keyboard input only reaches editable
fields that currently have input focus — tap the field on the phone first; otherwise the browser
shows `keyboard_no_focus`.

Evidence (real device, phone screenshot + browser screenshot):
[before](docs/android/01-phone-settings-before.jpg) /
[after](docs/android/02-phone-settings-after.jpg) /
[browser](docs/android/03-browser-remote-control.jpg) /
[swipe](docs/android/04-phone-swipe.jpg) /
[landscape](docs/android/05-browser-landscape.jpg) /
[accessibility guide](docs/android/06-accessibility-guide.jpg)

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

This project is licensed under the **MIT License** — free to use, modify, distribute and sell, even commercially, as long as the copyright notice is retained. See the [LICENSE](LICENSE) file for details.

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
| **macOS** | 🧪 实验版：CI 产出 Intel 与 Apple Silicon 两个 `.app`（签名 + 无头冒烟通过），真机交互未实测。下限 **Intel 10.14+ / Apple Silicon 15+** |
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
# 构建（Linux / Windows，纯 CMake；三方库与 FFmpeg 全自动从源码构建）
cd src && cmake --preset linux && cmake --build --preset linux        # Linux
cd src && cmake --preset windows-mingw && cmake --build --preset windows-mingw   # Windows

# 也可以用一键脚本（等价流程）
./src/linux_build_script/build_linux.sh          # 零参数自动探测 Qt
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

### Linux Wayland：systemd 服务 + 用户会话代理

Wayland 下只有登录用户会话内的进程才能采到真实桌面；root 服务单独运行时采到的是虚拟 X 屏幕（Xvnc），不是用户桌面。`--install` 会配置两个协作进程：

- `remotedesk.service`（系统级，root）：端口、配置、编码、锁屏判定与虚拟显示回落。
- `remotedesk-agent.service`（用户级，登录桌面用户，随登录自动启动）：PipeWire ScreenCast 采集 + 输入注入；通过 `$XDG_RUNTIME_DIR/remotedesk/session-agent.sock`（0600）与控制端通信，帧数据走 `/dev/shm/remotedesk-agent.<uid>.<pid>`。

说明：

- 采集优先链：会话代理 → Wayland（PipeWire）→ X11 → Xvnc。代理停止或授权被拒时，服务继续用回落源出画面，不会黑屏。
- 回落过渡：代理采集期间本地虚拟屏会被拆掉（不常驻一套空闲的 Xvnc 桌面）。因此**手工** `systemctl --user stop remotedesk-agent` 会重建 Xvnc + 桌面会话，约 10 秒内是过渡黑屏，之后自动回到回落桌面；崩溃（kill -9）几秒内就会重新接管，授权被拒则根本不拆虚拟屏，这两种情况都不会黑屏。
- 输入优先用用户会话内的 `/dev/uinput`；不可写时 install/启动日志会打印 `usermod -aG input <用户>` 提示（需下次登录生效，安装程序不会擅自改权限），输入自动降级到 root 侧注入。
- 坐标映射以代理上报的宽高为唯一基准，root 侧不再叠加补偿。
- portal 授权在支持 `restore_token` 时持久化到 `$XDG_STATE_HOME/remotedesk/portal_token`（0600，默认 `~/.local/state/remotedesk/portal_token`）；Mutter ScreenCast 无需弹窗。
- 总开关：给 `remotedesk.service` 设 `QTRD_DISABLE_SESSION_AGENT=1` 可退回旧的单进程形态。
- 重启：`systemctl restart remotedesk && systemctl --user restart remotedesk-agent`；代理崩溃后 3 秒自动重启。

### Windows 32 位：一条命令出两个包（Win7 6.1.2 + XP 3.4.8）

```bat
src\windows_build_script\build_32pair.bat          :: 两个包，先 Win7
src\windows_build_script\build_32pair.bat win7     :: 只出 FFmpeg 6.1.2
src\windows_build_script\build_32pair.bat xp       :: 只出 FFmpeg 3.4.8
```

产物为 `bin_win7_612\QtRemoteDesktop.exe`（FFmpeg 6.1.2）与 `bin_xp_348\QtRemoteDesktop.exe`（FFmpeg 3.4.8），结束时打印两个 exe 的**尺寸 + SHA256**。

脚本固化的构建规则（`build_onekey_build*.ps1` / `build.bat` / `build_32pair.bat` 中同样写明）：

1. **构建顺序：先 Win7（6.1.2），后 XP（3.4.8）。**
2. **每次构建前删 `build_output_win32\lib\*.a`。** 两棵 32 位树把静态库都写进同一个共享目录（`CMAKE_ARCHIVE_OUTPUT_DIRECTORY`），而 `libVideoEncoder.a` 里编进了 FFmpeg 版本相关的目标文件。后构建的树会直接链接另一棵树留下的归档：6.1.2 侧报 `undefined reference to 'avcodec_register_all'`；3.4.8 侧不报错，但产出的 exe **不含 FFmpeg**（`7,394,065` 字节，正常应约 `10.2 MB`，2026-11-04 实际发生）。
3. **出包即校验：** exe 必须含自身版本的 FFmpeg 标记（6.1.2 为 `av_channel_layout_default`，3.4.8 为 `avcodec_register_all`），且不能含另一版本的标记，同时必须含 openh264（`WelsCreateSVCEncoder`）。双目标脚本会自动做这一步。
4. 用打印出的 SHA256 与目标机上实际部署的 exe 比对，确认部署版就是本次出包。

### Android：一条命令出 debug APK（Qt 6 / arm64-v8a）

```bat
src\android_build_script\build_android.bat
```

脚本用 Qt 6 的 `android_arm64_v8a` 套件调用 `qt-cmake`（`QT_HOST_PATH=mingw_64`），固定
`ANDROID_ABI=arm64-v8a`、`USE_FFMPEG=ON`、`USE_WEBRTC=OFF`、`BUILD_THIRDPARTY=OFF`，构建
`apk` 目标，结束时打印 **APK 路径、大小、SHA256 和 `adb install` 命令**。

配置 CMake 之前，脚本会调用 `src\android_build_script\build_ffmpeg_android.ps1` 把
**FFmpeg 6.1.2 交叉编译成 arm64-v8a** 静态库，产物在
`build_output\ffmpeg_install-6.1.2-android-arm64`（`--enable-mediacodec --enable-jni
--enable-encoder=h264_mediacodec`、`-fPIC`、API 24、不含 openh264）。产物目录带版本与架构后缀，
与桌面 MinGW/pe-i386 那份严格隔离，绝不会互相误链；脚本按签名幂等，已是最新时秒退
（要重编用 `build_ffmpeg_android.ps1 -Force`）。首次交叉编译在 8 线程下大约 **6–12 分钟**，
APK 因静态链接 libavcodec/libavutil/libswscale 会增大 **约 0.7 MB**（实测 arm64-v8a debug
包 24,914,585 → 25,658,891 字节）。H.264 编码走手机
**MediaCodec 硬编**（NV12 系统内存输入，无需 JavaVM）；如果设备没有 H.264 编码器，
服务端会如实上报「视频模式不支持」，浏览器继续用 JPEG 图片模式，不会出现假能力。

脚本幂等：已有 `build_android\` 会重新配置并增量构建；要重来加 `-Clean`。
失败时只打印**第一条真实错误**（`FAILED:` / `CMake Error` / `error:`）和完整日志路径，
不会刷屏。

构建前置：

| 项目 | 要求 |
|---|---|
| Qt | Qt 6.x，带 **android_arm64_v8a** 套件 **和** 对应的 **mingw_64** 主机工具（`bin\qt-cmake.bat`） |
| Android SDK | platform-tools、build-tools、一个 platform、`cmake` + `ninja` |
| NDK | r27（优先 `27.1.12297006`，其它版本用 `-NdkRoot` 指定） |
| JDK | 17+，且设置 `JAVA_HOME`（Gradle 要求） |
| Gradle 镜像 | wrapper 已指向腾讯镜像；Maven Central 慢时可在 `%USERPROFILE%\.gradle\init.gradle` 里加阿里云仓库 |

环境变量覆盖：`QT_ANDROID_ROOT`、`ANDROID_SDK_ROOT`、`ANDROID_NDK_ROOT`；
脚本参数：`-QtRoot`、`-BuildDir`、`-NdkRoot`、`-Clean`。

安装与启动：

```bat
adb install -r build_android\remotedesk\android-build\build\outputs\apk\debug\android-build-debug.apk
adb shell am start -n com.radiometer.remotedesk/.MainActivity
```

然后在手机上点 **开始共享屏幕** → 系统弹窗选 **整个屏幕** → **立即开始**，
电脑浏览器打开 `http://<手机IP>:8080`（默认 `admin` / `admin`）。

**三件必须手动授权的事（无法绕过）：**

1. **屏幕录制授权** —— Android 14 每次 App 进程重启（重启手机、强行停止、重装）都要重新确认一次。
   App 内有「开始共享屏幕」按钮触发流程，系统弹窗必须点「立即开始」。
2. **无障碍服务** —— `设置 → 无障碍 → RemoteDesk 输入服务`（一次开启，重启后保留）。
   点击 / 滑动 / 长按 / 返回 / 主页 **以及键盘输入**都依赖它（Android 不允许普通应用注入按键）。
   未开启时浏览器会收到明确的 `input_status` 失败回执并显示原因，**事件不会被静默丢弃**。
3. **修改系统设置（`WRITE_SETTINGS`）** —— 仅 **App 在后台时切换横竖屏** 需要
   （即「手机放一边、全程用电脑浏览器操作」的典型用法）。未授权时方向只在 App 前台生效，
   浏览器会收到 `write_settings_required` 与授权路径。一次性授权：
   ```bash
   adb shell appops set com.radiometer.remotedesk android:write_settings allow
   ```
   或手机：`设置 → 应用 → RemoteDesk → 特殊权限 → 修改系统设置`。浏览器工具栏里的
   **「授权横屏」** 按钮（仅 Android 显示）会直接打开该页面（`ACTION_MANAGE_WRITE_SETTINGS`）；
   授权后浏览器自动隐藏按钮并提示已授权。副作用：系统 **自动旋转** 会被关闭（保持你选定的方向）；
   需要恢复传感器旋转时在快捷开关里重新打开。
   注意：如果手机前台是强制自身方向的界面（桌面、锁屏、部分视频应用），它会覆盖系统旋转锁，
   此时切换会超时并由浏览器显示 `orientation_timeout` 原因，请先切到允许旋转的应用。

**中文 / Unicode 文本输入（电脑与手机浏览器都支持）**：浏览器输入法组合过程中不发送任何按键；
`compositionend` 时把整段已上屏文字作为一条 `input_text` 消息发往服务端，Android 侧用无障碍
`ACTION_SET_TEXT` 直接写进当前聚焦输入框，不依赖手机输入法候选界面。英文 / 数字 / 符号仍走
逐键路径，两条路径共用同一份影子文本，因此连续输入「测试」「中文」「abc123中文」会按顺序合并，
不重复、不丢字。触屏设备上工具栏的 **「键盘」** 按钮用于主动唤起本机软键盘（默认点画面不会弹键盘），
其上屏文字走同一条路径。限制：只能写进 **当前有输入焦点的可编辑控件**；拒绝 `ACTION_SET_TEXT`
的受保护输入框回 `keyboard_key_failed`，没有焦点回 `keyboard_no_focus`（浏览器均会显示原因）；
手机自带候选词界面无法在电脑端显示/操作；快捷键与方向键仍不支持。

Android 的文件管理从虚拟根开始：`/sdcard`（共享存储）与 App 私有 `files` 目录，均可进入并显示
文件名与大小；App 无权限读取的目录（如受保护的系统路径）会返回明确的错误提示，而不是空白面板。
受分区存储限制，部分共享目录仍可能不可见，但同样会以权限错误提示，不会静默空白。

已支持：实时查看；点击 / 滑动 / 长按 / 返回 / 主页；**键盘文本写入当前聚焦输入框**（英文、数字、
空格、常见标点、回车、退格，以及**浏览器输入法上屏的中文 / 任意 Unicode 文本**，走 `input_text`）；
竖屏与横屏（画布和坐标映射跟随真实的 720x1612 ↔ 1612x720，且只有采集画面尺寸真的变化后才回执成功）；
App 退到后台后 8080 页面仍可访问（前台采集服务保活）。

明确不支持：键盘快捷键（Ctrl/Alt 组合）、方向键/功能键、手机自带输入法候选词界面（文本改由浏览器侧上屏）、
多指手势（缩放）、音频、文件传输、网页终端、无人值守。键盘只作用于当前有输入焦点的可编辑控件——
请先在手机画面上点中输入框，否则浏览器会收到 `keyboard_no_focus` 提示。

实机验收截图（手机截图 + 浏览器截图）：
[点击前](docs/android/01-phone-settings-before.jpg) /
[点击后](docs/android/02-phone-settings-after.jpg) /
[浏览器画面](docs/android/03-browser-remote-control.jpg) /
[滑动](docs/android/04-phone-swipe.jpg) /
[横屏](docs/android/05-browser-landscape.jpg) /
[无障碍指引](docs/android/06-accessibility-guide.jpg)

### 直接用 CMake 构建（不用一键脚本）

Qt 会自动探测（Windows 扫 `QT_ROOT`、`E:/programes/Qt`、`C:/Qt`、`%ProgramFiles%/Qt`、`~/Qt`；
Linux/macOS 扫 `/opt/Qt`、`~/Qt`），按编译器家族（MinGW/MSVC）、目标位宽、
MinGW 版本号（`mingw53_32` 对应 GCC 5.3）挑最匹配的套件。探测不准时再显式给 `CMAKE_PREFIX_PATH`。

preset 文件在 `src/CMakePresets.json`，`cmake --preset` 只读**当前目录**下的 preset，
所以要么在仓库根目录用 `-S src` 指过去，要么 `cd src`：

```bash
# 仓库根目录执行（推荐）
cmake -S src --preset windows-mingw        # Windows：MinGW + Qt 5.7 mingw53_32
cmake --build build --parallel 8           # 产物在 <repo>/build，可执行文件在 <repo>/bin
# Linux
cmake -S src --preset linux
cmake --build build --parallel 8

# 或者进到 src/ 用 build preset
cd src
cmake --preset windows-mingw && cmake --build --preset windows-mingw
```

或者不用 preset，直接给参数：

```bash
# Linux / macOS（Qt 不在常见位置时补 -DCMAKE_PREFIX_PATH=<Qt 前缀>）
cmake -S src -B build
cmake --build build --parallel 8

# Windows（MinGW + Qt5；工具链不在 PATH 时显式指定编译器，Qt 可交给自动探测）
cmake -S src -B build -G "MinGW Makefiles" ^
      -DCMAKE_MAKE_PROGRAM=E:\programes\Qt\QtLegacy\Tools\mingw530_32\bin\mingw32-make.exe ^
      -DCMAKE_C_COMPILER=E:\programes\Qt\QtLegacy\Tools\mingw530_32\bin\gcc.exe ^
      -DCMAKE_CXX_COMPILER=E:\programes\Qt\QtLegacy\Tools\mingw530_32\bin\g++.exe
cmake --build build --parallel 8
```

产物仍输出到 `bin/`（可执行文件）与 `build_output/`（静态库/三方库），与一键脚本一致。
`build_output/` 里的 openh264、FFmpeg 等都会由 `cmake --build` 自动从源码构建，无需先跑脚本。

| 选项 | 说明 |
|---|---|
| `BUILD_THIRDPARTY` | 非 MSVC 平台默认 ON：从源码构建 metaRTC / srtp / usrsctp / openh264（需 GNU make；MSVC 下强制 OFF） |
| `FFMPEG_VERSION` | FFmpeg 版本，默认 `6.1.2`（Win7 32 位、64 位、Linux、macOS 一律用它）；只有 Windows XP 32 位兼容构建显式传 `3.4.8`（`build.bat` 已自动传） |
| `BUILD_FFMPEG` | 对应版本的 `build_output/ffmpeg_install*` 缺失时默认 ON：由 CMake 构建自带 FFmpeg（`3.4.8` 用 `ffmpeg_install`，`6.1.2` 用 `ffmpeg_install-6.1.2`，均含 libopenh264 编码器）。有预编译产物时自动跳过 |
| `USE_FFMPEG` | OFF=自动探测（预编译产物放在对应版本的 `build_output/ffmpeg_install*`），ON=找不到即报错 |
| `USE_WEBRTC` | OFF=自动（有 FFmpeg 且 `BUILD_THIRDPARTY=ON` 时启用），ON=条件不满足即报错 |
| `BUILD_OUTPUT_DIR` / `BIN_OUTPUT_DIR` | 覆盖共享输出目录，便于多个构建目录并存 |

> MSVC 也能直接 `cmake -S src -B build`（自动挑 msvc 套件），但 metaRTC/srtp/openh264/FFmpeg
> 这些三方源码只支持 GNU make，会自动降级为「无 WebRTC、图片模式」编译。

> Windows 桌面捕获由 CMake 编译探测决定：工具链的 `dxgi1_2.h` 含 `IDXGIOutput1`
> 时启用 DXGI 桌面复制（MSVC、MinGW-w64 5+，如 Qt 5.15 自带的 mingw810）；
> 缺该接口的老头文件（Qt 5.7 的 mingw530）改用仓库内置的
> `dxgi1_2_compat.h` 补齐声明，同样启用 DXGI（日志打印 `DXGI enabled via
> built-in compat declarations`）。运行期 Win7/XP、初始化失败、连续失败、帧纹理
> 非 BGRA8（HDR 桌面上个别驱动给 R16G16B16A16_FLOAT）或复制通道恒出黑帧时
> 自动回退 GDI。`D3D11CreateDevice` 从 `d3d11.dll` 动态取（其余都是 COM 虚表
> 调用），exe 不静态导入 d3d11/dxgi——否则没有这两个 DLL 的 Windows XP 会在
> 加载 exe 时就失败。

> FFmpeg 现编时会自动探测 `nasm`/`yasm`（含 Git 的 `usr/bin`），并打印
> `FFmpeg: x86asm enabled via ...`；缺失时退纯 C 实现（swscale RGB→YUV 明显变慢）。

> `build_output/` 与 `bin/` 默认被所有构建目录共用：同时保留多套配置（如开关 WebRTC）时，请显式指定不同的 `BUILD_OUTPUT_DIR`/`BIN_OUTPUT_DIR`，否则后一次构建会覆盖前一次的同名产物。
>
> 例外是 32 位双目标（Win7 6.1.2 + XP 3.4.8）：它们**有意**共用 `build_output_win32`（FFmpeg 产物按版本分目录 `ffmpeg_install` / `ffmpeg_install-6.1.2`），因此必须遵守「先 Win7 后 XP + 每次构建前清空 `build_output_win32/lib/*.a`」。`build_32pair.bat` 已内置这两条规则，手工跑 cmake 时要自己删。

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

**Q: Apple Silicon 为什么要 macOS 15 起？**
A: 不是代码限制，是依赖来源限制：arm64 腿的 Qt 及其传递依赖（freetype/openssl/glib/webp…）直接取 Homebrew bottle，这些 dylib 的 `minos` 全是 15.0。Intel 腿用 aqt 官方 Qt，产物里除 openssl 外都是 10.13/10.14。要把 arm64 下探到 11.0 就得在 CI 上自编译 Qt 和整条依赖链。

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

本项目采用 **MIT 许可证** —— 可免费用于使用、修改、分发甚至商业销售，只需保留版权声明。详见 [LICENSE](LICENSE) 文件。
