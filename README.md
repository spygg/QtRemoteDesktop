# Qt Remote Desktop

基于 Qt 和 WebSocket 的远程桌面控制应用,支持屏幕捕获、视频编码、文件传输和网络通信。

## 功能特性

- **屏幕捕获**: 支持 Windows、Linux、macOS 和 Android 平台的屏幕捕获
  - Windows: 使用 DirectX 11 和 DXGI API
  - Linux: 使用 X11、XTest、XDamage、XComposite、XRender 库
  - macOS: 使用 CoreGraphics
- **视频编码**: 可选的 FFmpeg H.264 视频编码
- **WebSocket 通信**: 使用 WebSocket 进行实时双向通信
- **SSL/TLS 加密**: 支持安全的 SSL/TLS 加密连接
- **输入控制**: 支持鼠标和键盘输入的远程控制，支持中文输入法
- **文件传输**: 支持客户端与服务端之间的文件上传和下载
- **用户认证**: 支持用户名密码认证机制
- **远程 Shell**: 支持交互式命令行远程访问
- **双模式支持**:
  - 视频模式: 使用 FFmpeg 编码视频流
  - 图片模式: 使用 JPEG 压缩单帧图像
- **单实例运行**: 支持应用程序单实例运行
- **Windows 服务**: 支持以 Windows 服务方式运行
- **系统睡眠阻止**: 运行时阻止系统进入睡眠状态

## 项目结构

```
qtremotedesktop/
├── bin/                      # 编译输出目录(已忽略)
├── src/
│   ├── CMakeLists.txt        # 根 CMake 配置文件
│   ├── linuxqt5.sh           # Linux Qt5 一键构建脚本
│   ├── linuxqt6.sh           # Linux Qt6 一键构建脚本
│   ├── onekeydeploy.sh       # 一键部署脚本
│   └── remotedesk/
│       ├── CMakeLists.txt    # 主程序 CMake 配置
│       ├── main.cpp          # 程序入口
│       ├── resources.qrc     # Qt 资源文件
│       ├── res/              # 资源目录
│       │   ├── html/         # Web 客户端 HTML 文件
│       │   │   ├── index.html        # 远程桌面主页面
│       │   │   ├── login.html        # 登录页面
│       │   │   ├── shell.html        # 远程 Shell 页面
│       │   │   └── user-management.html # 用户管理页面
│       │   ├── sslperm/      # SSL 证书和密钥文件
│       │   │   ├── cacert.crt        # 证书文件
│       │   │   └── privkey.pem       # 私钥文件
│       │   ├── main.ico      # 应用图标
│       │   ├── main.png      # 应用图标
│       │   ├── main.rc       # 资源脚本
│       │   └── uac.manifest  # UAC 清单
│       ├── server/
│       │   ├── authmanager/          # 用户认证管理器
│       │   ├── filetransferservice/  # 文件传输服务
│       │   ├── helper_process/       # 辅助进程(用于服务模式)
│       │   ├── inputmanger/          # 输入管理器(跨平台)
│       │   │   ├── inputmanager_win.cpp   # Windows 输入实现
│       │   │   ├── inputmanager_linux.cpp # Linux 输入实现
│       │   │   ├── inputmanager_mac.cpp   # macOS 输入实现
│       │   │   └── inputmanager_android.cpp # Android 输入实现
│       │   ├── rdpserver/            # RDP 服务器主类
│       │   ├── screencapturer/       # 屏幕捕获器(跨平台)
│       │   │   ├── screencapturer_win.cpp   # Windows 捕获实现
│       │   │   ├── screencapturer_linux.cpp # Linux 捕获实现
│       │   │   ├── screencapturer_mac.cpp   # macOS 捕获实现
│       │   │   └── screencapturer_android.cpp # Android 捕获实现
│       │   ├── secure_input_process/ # 安全输入进程
│       │   ├── service/              # 服务管理(跨平台)
│       │   │   ├── windows_service.cpp     # Windows 服务实现
│       │   │   └── linux_service.cpp       # Linux 服务实现
│       │   ├── shell/                # 交互式 Shell
│       │   │   ├── shell_win.cpp           # Windows Shell 实现
│       │   │   ├── shell_linux.cpp         # Linux Shell 实现
│       │   │   └── shell_mac.cpp           # macOS Shell 实现
│       │   ├── singleapplication/    # 单实例应用
│       │   ├── videoencoder/         # 视频编码器(需要 FFmpeg)
│       │   └── websocketserver/      # WebSocket 服务器
│       └── thridparty/
│           ├── startup/              # 开机自启
│           │   ├── startup_windows.cpp    # Windows 自启实现
│           │   └── startup_linux.cpp      # Linux 自启实现
│           └── systemsleepblocker/   # 系统睡眠阻止
│               ├── systemsleepblocker_win.cpp   # Windows 实现
│               ├── systemsleepblocker_linux.cpp # Linux 实现
│               ├── systemsleepblocker_mac.mm    # macOS 实现
│               └── systemsleepblocker_android.cpp # Android 实现
├── .gitignore               # Git 忽略配置
└── README.md                # 项目说明文档
```

## 环境要求

### Windows 平台
- Qt 5.x 或 Qt 6.x
- Visual Studio 2019 或更高版本(用于 MSVC 编译器)
- CMake 3.15+
- DirectX 11 SDK(通常随 Windows SDK 一起安装)
- FFmpeg(可选,用于视频编码模式)

### Linux 平台
- Qt 5.x 或 Qt 6.x
- GCC 编译器
- CMake 3.15+
- X11 开发库: `libx11-dev`, `libxtst-dev`, `libxdamage-dev`, `libxcomposite-dev`, `libxrender-dev`
- FFmpeg(可选,用于视频编码模式)

### macOS 平台
- Qt 5.x 或 Qt 6.x
- Xcode
- CMake 3.15+
- FFmpeg(可选,用于视频编码模式)

### Android 平台
- Qt 6.x with Android support
- Android SDK and NDK
- CMake 3.15+

## 编译说明

### Windows 平台

1. 安装 Qt(推荐使用 Qt Online Installer)
2. 打开 Qt Creator,打开 `src/CMakeLists.txt`
3. 配置构建套件(选择 MSVC 编译器)
4. 点击构建

或使用命令行编译:

```bash
# 设置 Qt 环境
set PATH=C:\Qt\你的Qt版本\msvc版本\bin;%PATH%

# 创建构建目录
mkdir build
cd build

# 使用 CMake 生成构建文件
cmake ..\src -DCMAKE_PREFIX_PATH=C:\Qt\你的Qt版本\msvc版本

# 编译
cmake --build . --config Release
```

### Linux 平台

1. 安装依赖:

```bash
# Ubuntu/Debian
sudo apt-get install qtbase5-dev qtwebsockets5-dev \
    libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev

# Fedora/RHEL
sudo dnf install qt5-qtbase-devel qt5-qtwebsockets-devel \
    libX11-devel libXtst-devel libXdamage-devel libXcomposite-devel libXrender-devel
```

2. 使用一键构建脚本:

```bash
# Qt5
./src/linuxqt5.sh

# Qt6
./src/linuxqt6.sh
```

或手动编译:

```bash
mkdir build
cd build

# Qt5
cmake ../src -DCMAKE_PREFIX_PATH=/opt/Qt/5.15.2/gcc_64

# Qt6
cmake ../src -DCMAKE_PREFIX_PATH=/opt/Qt/6.x.x/gcc_64

cmake --build .
```

### 启用 FFmpeg 视频编码

要启用 FFmpeg 视频编码功能,需要:

1. 下载 FFmpeg 预编译库并放置到 `src/remotedesk/thridparty/ffmpeg/` 目录
   - Windows: 解压到 `windows/` 子目录
   - Linux: 解压到 `linux/` 子目录

2. 使用 CMake 时添加 `-DUSE_FFMPEG=ON`:

```bash
cmake ../src -DUSE_FFMPEG=ON
```

FFmpeg 库文件结构:

```
src/remotedesk/thridparty/ffmpeg/
├── windows/
│   ├── include/     # FFmpeg 头文件
│   ├── lib/         # FFmpeg 静态库和导入库(.lib, .a)
│   └── bin/         # FFmpeg DLL(运行时需要)
└── linux/
    ├── include/     # FFmpeg 头文件
    └── lib/         # FFmpeg 共享库
```

## 运行

编译完成后,可执行文件位于 `bin/` 目录。

### 正常运行

```bash
# Windows
bin\QtRemoteDesktop.exe

# Linux
./bin/QtRemoteDesktop
```

### 命令行选项

```bash
# 禁用 SSL/TLS(使用明文 HTTP/WS)
QtRemoteDesktop.exe --no-ssl

# 查看帮助
QtRemoteDesktop.exe --help
```

### Windows 服务模式

```bash
# 安装服务
bin\QtRemoteDesktop.exe --install

# 启动服务
net start QtRemoteDesktop

# 停止服务
net stop QtRemoteDesktop

# 卸载服务
bin\QtRemoteDesktop.exe --uninstall
```

默认端口:
- HTTP 服务: 8080
- WebSocket 服务: 8081(HTTP 端口 + 1)

## 使用说明

1. 启动服务器程序
2. 打开浏览器访问 `http://服务器地址:8080`
3. 输入用户名和密码进行认证(默认: admin/admin)
4. 在网页中可以查看远程桌面并控制

### 远程 Shell

登录后点击顶部导航的"Shell"标签,即可进入远程命令行界面:
- 支持交互式命令执行
- 支持 Tab 键自动补全
- 支持鼠标右键菜单(复制/粘贴)
- 支持 Ctrl+V 粘贴

### 文件传输

客户端可以通过网页界面进行文件上传和下载:
1. 点击顶部导航的"文件传输"标签
2. 上传: 点击"选择文件"或拖拽文件到上传区域
3. 下载: 点击文件列表中的下载按钮

## 配置

配置文件位于程序运行目录下的 `server_config.json`:

```json
{
    "ssl": false,
    "httpPort": 8080,
    "console": false,
    "fps": 30,
    "quality": 60,
    "scale": 75,
    "users": {
        "admin": "2923be84e16cd6ae529049f1f1bbe9eb:3400f2e7a3b6612bf40e6762b7d40a9ac64217fb4e039cf953c5b17f9ae071e7"
    }
}
```

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `ssl` | 是否启用 SSL/TLS | false |
| `httpPort` | HTTP 服务端口 | 8080 |
| `console` | 是否显示控制台窗口 | false |
| `fps` | 屏幕捕获帧率(1-60) | 30 |
| `quality` | JPEG 压缩质量(10-100) | 60 |
| `scale` | 画面缩放比例(10-100) | 75 |

### SSL 证书

项目使用自签名证书,位于 `src/remotedesk/res/sslperm/` 目录:
- `cacert.crt` - 证书文件
- `privkey.pem` - 私钥文件

如需使用自己的证书,替换这两个文件并重新编译。

### 用户认证

默认用户名和密码:
- 用户名: `admin`
- 密码: `admin`

如需修改认证凭据,请修改 `AuthManager` 类中的认证逻辑。

### 模式切换

客户端可以通过网页界面切换显示模式:
- **视频模式**: 使用 FFmpeg 编码(需要启用 FFmpeg)
- **图片模式**: 使用 JPEG 压缩(默认)

## 技术栈

- **Qt**: 跨平台应用程序框架
- **CMake**: 跨平台构建系统
- **WebSocket**: 实时双向通信协议
- **FFmpeg**: 多媒体处理库(可选)
- **DirectX 11**: Windows 平台屏幕捕获
- **X11**: Linux 平台屏幕捕获
- **CoreGraphics**: macOS 平台屏幕捕获
- **Windows API**: Windows 平台输入模拟和服务管理
- **SendInput**: Windows 平台键盘鼠标输入模拟
- **SendMessage**: 锁屏状态下的输入模拟

## 注意事项

1. FFmpeg 二进制文件和 DLL 已被 .gitignore 忽略,需要自行下载
2. 编译输出目录 `bin/` 已被忽略
3. Windows 平台需要 MSVC 编译器
4. Linux 平台需要 X11 相关开发库
5. SSL 证书使用自签名证书,浏览器可能会有安全警告
6. 服务模式下,程序会自动启动辅助进程处理用户会话
7. 系统默认阻止睡眠,确保远程控制稳定性

## 许可证

请查看项目许可证文件。

## 贡献

欢迎提交 Issue 和 Pull Request。
