# Qt Remote Desktop

基于 Qt + WebSocket 的远程桌面控制应用，支持屏幕捕获、H.264 视频编码、WebRTC/SRTP 低延迟推流、输入控制、文件传输与远程 Shell。

---

## 平台支持现状（2026-09 代码审查确认）

| 平台 | 状态 | 说明 |
|------|------|------|
| **Windows**（MSVC / MinGW） | ✅ 完整 | DXGI/D3D11 捕获 + GDI 回退；SendInput 输入；Windows 服务模式 |
| **Linux（X11 会话）** | ✅ 可用 | X11 + XDamage/XComposite/XRender 捕获；XTest 输入；PTY Shell；systemd 服务 |
| **Linux（Wayland 会话）** | ⚠️ 受限 | 需 `libpipewire-0.3` 才编译 Wayland 捕获；无 PipeWire 时只有 X11 路径（XWayland 下画面更新很慢） |
| **macOS** | ❌ 不可构建 | `Service`/`Shell`/`ScreenCapturer` 缺 Apple 平台实现（链接失败）；且无 Info.plist 屏幕录制权限声明 |
| **Android** | ❌ 不可用 | 捕获/输入均为 stub；无 Android 分支的 CMake/脚本配置 |

> 结论：当前真正可用的组合是 **Windows** 与 **Linux X11**。macOS / Android 属于"代码骨架存在但未落地"。

---

## 功能特性

**屏幕捕获**
- Windows：DirectX 11 / DXGI（`_WIN32_WINNT=0x0A00` 时启用），失败回退 GDI
- Linux：X11 + XDamage 增量 + XComposite/XRender；可选 Wayland（PipeWire）
- 多显示器枚举与切换（Windows / Linux 已实现）

**视频传输（双通道）**
- WebSocket 通道：服务端推送 Annex-B H.264，前端经 fMP4（ftyp+moov+mvex+moof+mdat）封装后喂 MSE
- WebRTC 通道：metaRTC 库做 ICE/DTLS/SRTP，RTP over UDP，浏览器原生解码（延迟最低）

**视频编码**
- 软件编码：FFmpeg + libopenh264（H.264）
- 硬件编码：RK3588 Rockchip MPP，初始化失败自动回退软编
- 过载保护：按编码耗时 EMA 动态降码率

**其他**
- 输入控制（鼠标/键盘/滚轮，Windows 支持 Unicode 字符注入）
- 剪贴板同步、文件传输（上传/下载/目录打包下载）
- 交互式远程 Shell（Linux/macOS 为真 PTY，支持窗口尺寸同步与 Tab 补全；Windows 为管道式非 PTY）
- 用户认证（用户名/密码 + token 会话）、用户管理页面
- 单实例运行、HTTPS/WSS、系统睡眠阻止
- Windows 服务模式（Session 0 辅助进程注入用户会话）
- Linux systemd 服务模式（`--install` / `--uninstall` / `--service`）

---

## 项目结构

```
qtremotedesktop/
├── bin/                          # 最终可执行文件（已忽略）
├── build/                        # CMake 中间目录（已忽略）
├── build_output/                 # 第三方库产物：ffmpeg_install / openh264_install / mpp_install
├── cmake/                        # FindFFmpeg 等 CMake 模块
└── src/
    ├── CMakeLists.txt            # 根 CMake（选项：USE_FFMPEG / USE_WEBRTC / BUILD_THIRDPARTY）
    ├── linux_build_script/
    │   ├── build_linux.sh        # Linux 一键编译（依赖自检 + 增量构建）
    │   ├── linuxqt5.sh           # 兼容入口，转发到 build_linux.sh
    │   └── onekeydeploy.sh       # 一键打包（拷贝 Qt 依赖 + 生成启动脚本/.desktop）
    ├── windows_build_script/     # build.bat + build_*.ps1
    ├── mac_build_script/         # build_mac.sh（当前存在链接级缺口）
    └── remotedesk/
        ├── main.cpp              # 程序入口
        ├── resources.qrc / res/  # Web 客户端（index/login/shell/user-management）、SSL 证书、图标
        ├── server/
        │   ├── authmanager/      # 认证与用户管理
        │   ├── clipboardservice/ # 剪贴板同步
        │   ├── filetransferservice/
        │   ├── inputmanger/      # 输入模拟（win/linux/mac/android）
        │   ├── rdpserver/        # 主服务类（模式切换、帧分发、WebRTC 信令）
        │   ├── screencapturer/   # 屏幕捕获（win/linux/mac/android + wayland）
        │   ├── service/          # 服务/守护（windows_service / linux_service）
        │   ├── shell/            # 交互式 Shell（win/linux/mac）
        │   ├── singleapplication/
        │   ├── videoencoder/     # FFmpeg 软编 + MPP 硬编
        │   ├── webrtcsession/    # metaRTC WebRTC 会话
        │   ├── websocketserver/  # WS/HTTP 服务
        │   └── helper_process/、secure_input_process/
        └── thridparty/           # metaRTC / srtp / usrsctp / openh264 / mpp / breakpad / FFmpeg 源码
```

---

## 环境要求

### Windows

| 依赖 | 说明 |
|------|------|
| Qt 5.x（MinGW 或 MSVC） | 需包含 **QtWebSockets** 组件 |
| CMake 3.15+ | 安装时勾选 Add to PATH |
| Git Bash | 构建脚本需要 `bash.exe` |
| mingw32-make | Qt 自带，`<Qt>\Tools\mingw*\bin\` |
| Windows SDK | DirectX 11 / DXGI 头文件 |

可用环境变量覆盖自动探测：`QT_BASE_DIR`、`GIT_BASH`、`MINGW_MAKE`。

### Linux

| 依赖 | 用途 | Debian/Ubuntu | RHEL/Fedora |
|------|------|---------------|-------------|
| 编译工具 | gcc/g++/make | `build-essential` | `gcc gcc-c++ make` |
| CMake / pkg-config | 构建系统 | `cmake pkg-config` | `cmake pkgconfig` |
| Qt5 + WebSockets | 核心框架（**必需 WebSockets**） | `qtbase5-dev qtwebsockets5-dev` | `qt5-qtbase-devel qt5-qtwebsockets-devel` |
| libX11 | 屏幕捕获基础 | `libx11-dev` | `libX11-devel` |
| libXtst | 输入模拟（XTest） | `libxtst-dev` | `libXtst-devel` |
| libXdamage | 增量区域捕获 | `libxdamage-dev` | `libXdamage-devel` |
| libXcomposite | 窗口合成捕获 | `libxcomposite-dev` | `libXcomposite-devel` |
| libXrender | 渲染捕获 | `libxrender-dev` | `libXrender-devel` |
| libXfixes | 光标/区域扩展 | `libxfixes-dev` | `libXfixes-devel` |
| OpenSSL | WSS + metaRTC DTLS | `libssl-dev` | `openssl-devel` |
| zlib | 压缩 | `zlib1g-dev` | `zlib-devel` |
| ALSA | metaRTC 音频依赖 | `libasound2-dev` | `alsa-lib-devel` |
| libpcap | metaRTC 网络依赖 | `libpcap-dev` | `libpcap-devel` |
| **可选** PipeWire | Wayland 抓屏（缺失则仅 X11） | `libpipewire-0.3-dev` | `pipewire-devel` |
| **可选** mlocate | Qt 路径探测加速 | `mlocate` | `mlocate` |
| **可选** libdrm | RK3588 MPP 硬编 | `libdrm-dev` | `libdrm-devel` |

一键安装：

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential cmake pkg-config \
    qtbase5-dev qtwebsockets5-dev \
    libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev libxfixes-dev \
    libssl-dev zlib1g-dev libasound2-dev libpcap-dev \
    libpipewire-0.3-dev mlocate

# RHEL / Fedora / Anolis
sudo dnf install -y gcc gcc-c++ make cmake pkgconfig \
    qt5-qtbase-devel qt5-qtwebsockets-devel \
    libX11-devel libXtst-devel libXdamage-devel libXcomposite-devel libXrender-devel libXfixes-devel \
    openssl-devel zlib-devel alsa-lib-devel libpcap-devel \
    pipewire-devel mlocate

# Arch
sudo pacman -S --needed base-devel cmake pkgconf qt5-base qt5-websockets \
    libx11 libxtst libxdamage libxcomposite libxrender libxfixes openssl zlib alsa-lib libpcap \
    pipewire mlocate
```

构建前自检依赖（会按发行版打印缺失包的安装命令）：

```bash
./src/linux_build_script/build_linux.sh --check-deps
```

### macOS

当前**不可构建**，详见"平台支持现状"。依赖仅供参考：`brew install cmake pkg-config`，Qt5/Qt6 通过 `-Q` 指定。

---

## 编译

脚本均为**增量构建**：默认不删除 `build/`、`build_output/`、FFmpeg/MPP 的中间目录，只有显式 `--clean` 才全量重建。

```bash
# Linux（推荐，零参数自动探测 Qt）
./src/linux_build_script/build_linux.sh                    # 自动探测 Qt
./src/linux_build_script/build_linux.sh -Q /opt/Qt/5.15.2/gcc_64
./src/linux_build_script/build_linux.sh -j 8               # 并行数
./src/linux_build_script/build_linux.sh --skip-ffmpeg      # 复用已有 FFmpeg 产物
./src/linux_build_script/build_linux.sh --clean            # 全量重建

# Windows
src\windows_build_script\build.bat

# macOS（存在已知链接级缺口）
./src/mac_build_script/build_mac.sh
```

手动 CMake：

```bash
cmake -S src -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/Qt/5.15.2/gcc_64 \
  -DBUILD_THIRDPARTY=ON
cmake --build build -j$(nproc)
```

| CMake 选项 | 默认 | 说明 |
|------------|------|------|
| `BUILD_THIRDPARTY` | ON | 从源码构建 metaRTC / srtp / usrsctp / openh264 |
| `USE_FFMPEG` | OFF（自动探测） | 强制要求 FFmpeg，缺失则报错 |
| `USE_WEBRTC` | OFF（自动探测） | WebRTC 推流（依赖 FFmpeg 编码） |
| `CMAKE_PREFIX_PATH` | — | Qt 安装前缀 |

产物：可执行文件 `bin/QtRemoteDesktop`，调试符号 `bin/QtRemoteDesktop.debug`，第三方库 `build_output/`。

---

## 运行

```bash
# Linux
./bin/QtRemoteDesktop

# Windows
bin\QtRemoteDesktop.exe
```

| 选项 | 说明 |
|------|------|
| `--no-ssl` | 禁用 HTTPS/WSS，使用明文 HTTP/WS |
| `--install` | 安装系统服务（Windows 服务 / Linux systemd） |
| `--uninstall` | 卸载系统服务 |
| `--service` | 以服务模式运行（由服务管理器调用） |
| `--helper` | Windows 辅助进程模式（注入用户会话，由服务自动拉起） |
| `--help` | 查看帮助 |

- 默认端口：HTTP `8080`，WebSocket `8081`（HTTP 端口 + 1）
- 日志：`<程序目录>/logs/YYYY-MM-DD.txt`
- 默认账号：`admin` / `admin`

### 配置文件

程序运行目录下的 `server_config.json`（首次运行自动生成）：

```json
{
    "ssl": false,
    "httpPort": 8080,
    "console": false,
    "fps": 30,
    "quality": 60,
    "scale": 75,
    "users": { "admin": "<salt>:<hash>" }
}
```

| 配置项 | 说明 | 默认值 | 范围 |
|--------|------|--------|------|
| `ssl` | 启用 SSL/TLS | false | — |
| `httpPort` | HTTP 服务端口 | 8080 | — |
| `console` | 显示控制台窗口 | false | — |
| `fps` | 捕获帧率 | 30 | 1–60 |
| `quality` | JPEG 质量 | 60 | 10–100 |
| `scale` | 画面缩放 | 75 | 10–100 |
| `users` | 用户表（salt:hash） | admin | — |

SSL 使用 `res/sslperm/` 下的自签名证书，浏览器会有安全警告；替换 `cacert.crt` / `privkey.pem` 并重新编译可换成自己的证书。

---

## 使用说明

1. 启动服务端，浏览器访问 `http://<服务器地址>:8080`
2. 用 `admin` / `admin` 登录
3. 顶部可切换显示模式：
   - **自动 / WebRTC**：浏览器原生解码 RTP（延迟最低，推荐 Chrome/Edge）
   - **视频（FFmpeg/MSE）**：WS 推送 H.264 + MSE 播放，依赖浏览器 `MediaSource`（`video/mp4; codecs="avc1.*"`）
   - **图片**：JPEG 逐帧，兼容性最好、带宽最高

浏览器要求：WebRTC 路径由浏览器原生解码；WS 视频路径经 MSE 播放（无需 WebCodecs）。

---

## 故障排查

| 现象 | 排查方向 |
|------|----------|
| 页面黑屏 | 看 `bin/logs/*.txt`：确认有 `VideoEncoder: FORCE-KF` 与 `sendFrame`；静态桌面依赖关键帧引导，服务端已做周期 IDR；切换模式重试 |
| Wayland 下无画面 | 需安装 `libpipewire-0.3-dev` 后重新 CMake（日志出现 `PipeWire found`）；或改用 Xorg(X11) 会话 |
| Linux 服务模式抓不到屏 | 服务默认拿不到 `DISPLAY`/`XAUTHORITY`，需在运行环境中显式注入（当前 `detectUserX11Env` 仅处理 DISPLAY/XAUTHORITY，不支持 Wayland） |
| RK3588 编码慢 | 日志 `mpp_venc_kcfg_init failed` 表示 MPP 硬编不可用，已自动回退软件编码（1080p30 软编会明显占用 CPU） |
| 端口占用 | 改 `server_config.json` 的 `httpPort`（WS 端口自动 +1） |
| 编译报找不到 X11 头 | 先跑 `build_linux.sh --check-deps`，按提示安装 X11 系列开发库 |

---

## 代码审查修复记录

首轮全项目审查发现的问题已按下表修复，并在 RK3588（`192.168.1.90`）实测通过。
验证方式：远端增量编译（`make -j4`，EXIT=0）+ 浏览器端到端冒烟（WebRTC 与 ffmpeg/MSE
两种模式均出图，解码计数持续递增、丢帧 0）。

| 状态 | 级别 | 问题 | 修复要点 |
|------|------|------|----------|
| ✅ 已修复 | **P0** | 无捕获环境时 `/api/shell/exec`、`/api/shell/ws` 跳过鉴权 | `skipAuth` 收窄为仅 `/`、`/shell` 静态页；`onShellConnected` 恒校验 token |
| ✅ 已修复 | **P0** | `sanitizeFilePath` 不做根目录约束，可任意读写 | 新增 `setRootPath()` 根约束 + `canonicalFilePath` 解析软链；越界返回空并由调用方判失败 |
| ✅ 已修复 | **P0** | `onEncodedFrame` 遍历 `webrtcSessions_` 迭代器失效 | 先快照 key 列表，再按 key 查表，已移除会话自然跳过 |
| ✅ 已修复 | **P0** | `VideoEncoder::shutdown` 用 `terminate()` 强杀线程 | 改为 `requestInterruption()` + `quit()` + `wait(8000)`，仅在确认退出后释放资源 |
| ✅ 已修复 | **P0** | 分辨率/输出切换后未重建编码器（`sws_scale` 越界读） | 尺寸不匹配即丢帧并节流重建（3s 冷却）；切换成功后清缓存并 reinit |
| ✅ 已修复 | **P0** | 静态画面下 ffmpeg/MSE 完全冻结（只出首帧） | 周期关键帧定时器不再限定 WebRTC；`request_keyframe` 强制产新 IDR（`pumpKeyframe(true)`） |
| ✅ 已修复 | **P0** | 静止降频后永久卡在 4fps，画面变化也不恢复 | 移除 `interval() < 250` 守卫陷阱，改为显式 `enterIdleThrottle()`/`leaveIdleThrottle()`（Win/Linux/macOS 同步） |
| ✅ 已修复 | **P1** | WebRTC 启动失败后 `webrtcExcluded_` 未回滚 | 新增 `failNoEncoder` 统一回滚排除列表后再下发 `failed` |
| ✅ 已修复 | **P1** | 断开连接未清理 `clientTokens_`；上传残留 `QFile` | `onSocketDisconnected` 同步移除 token；析构与覆盖时关闭句柄 |
| ✅ 已修复 | **P1** | MPP 缩放用编码尺寸而非源尺寸；重试路径泄漏 `MppPacket` | `initialize` 区分 src/enc 尺寸；重试使用局部 packet 并 deinit 无效包 |
| ✅ 已修复 | **P1** | RTP 时间戳固定从 0 起、64 位不截断 | 随机初始值 + 32 位回绕；时间戳步进按实际 fps（`90000/fps`） |
| ✅ 已修复 | **P1** | `QTimer::singleShot` 无 context（UAF 风险） | 补 context 对象 `&app` |
| ✅ 已修复 | **P1** | breakpad CMake 硬编码 `Qt5::Core`，Qt6 全平台 configure 失败 | 改为 `Qt${QT_VERSION_MAJOR}::Core` |
| ⏳ 未处理 | **P2** | 开机自启模块在 CMake 中被注释（全平台不可用） | 需产品决策：是否恢复 `thridparty/startup` |
| ⏳ 未处理 | **P2** | macOS / Android 存在链接级缺口与 stub 实现 | 需目标平台实机才能验证与补齐 |

---

## 技术栈

- **Qt 5/6**：Core / Gui / Widgets / Network / WebSockets
- **CMake**：跨平台构建，`BUILD_THIRDPARTY` 从源码构建第三方
- **FFmpeg + openh264**：H.264 软编码；**Rockchip MPP**：RK3588 硬编码
- **metaRTC**：WebRTC（ICE / DTLS / SRTP / RTP）
- **DirectX 11 / DXGI**（Windows）、**X11 + PipeWire**（Linux）、**CoreGraphics**（macOS 骨架）

## 注意事项

1. FFmpeg / openh264 / MPP 由 `BUILD_THIRDPARTY` 从源码构建，产物在 `build_output/`
2. `bin/`、`build/`、`build_output/` 均已忽略
3. 服务模式下 Windows 会自动拉起辅助进程进入用户会话
4. SSL 为自签名证书，浏览器会提示不安全
5. macOS / Android 目前不可构建（见"平台支持现状"）

## 许可证

请查看项目许可证文件。

## 贡献

欢迎提交 Issue 和 Pull Request。
