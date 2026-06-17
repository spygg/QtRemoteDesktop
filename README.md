# Qt Remote Desktop

基于 Qt 和 WebSocket 的远程桌面控制应用,支持屏幕捕获、视频编码、文件传输和网络通信。

## 功能特性

- **屏幕捕获**: 支持 Windows 和 Linux 平台的屏幕捕获
  - Windows: 使用 DirectX 11 和 DXGI API
  - Linux: 使用 X11、XTest、XDamage、XComposite、XRender 库
- **视频编码**: 可选的 FFmpeg H.264 视频编码
- **WebSocket 通信**: 使用 WebSocket 进行实时双向通信
- **SSL/TLS 加密**: 支持安全的 SSL/TLS 加密连接
- **输入控制**: 支持鼠标和键盘输入的远程控制
- **文件传输**: 支持客户端与服务端之间的文件上传和下载
- **用户认证**: 支持用户名密码认证机制
- **双模式支持**:
  - 视频模式: 使用 FFmpeg 编码视频流
  - 图片模式: 使用 JPEG 压缩单帧图像
- **单实例运行**: 支持应用程序单实例运行
- **Windows 服务**: 支持以 Windows 服务方式运行

## 项目结构

```
qtremotedesktop/
├── bin/                      # 编译输出目录(已忽略)
├── src/
│   ├── main.cpp             # 程序入口
│   ├── qtremotedesktop.pro  # Qt 项目文件
│   ├── resources.qrc        # Qt 资源文件
│   ├── html/                # Web 客户端 HTML 文件
│   ├── server/
│   │   ├── rdpserver.h/cpp          # RDP 服务器主类
│   │   ├── websocketserver.h/cpp    # WebSocket 服务器
│   │   ├── screencapturer.h/cpp     # 屏幕捕获器基类
│   │   ├── screencapturer_win.cpp   # Windows 平台实现
│   │   ├── screencapturer_linux.cpp # Linux 平台实现
│   │   ├── inputmanager.h/cpp       # 输入管理器基类
│   │   ├── inputmanager_win.cpp     # Windows 输入实现
│   │   ├── inputmanager_linux.cpp   # Linux 输入实现
│   │   ├── videoencoder.h/cpp       # 视频编码器(需要 FFmpeg)
│   │   ├── authmanager.h/cpp        # 用户认证管理器
│   │   ├── filetransferservice.h/cpp # 文件传输服务
│   │   └── singleapplication.h/cpp  # 单实例应用
│   ├── service/
│   │   ├── main.cpp         # Windows 服务入口
│   │   ├── service.h/cpp    # Windows 服务实现
│   │   └── ServiceInstaller # Windows 服务安装脚本
│   ├── sslperm/             # SSL 证书和密钥文件
│   └── thridparty/
│       └── ffmpeg/          # FFmpeg 第三方库(二进制文件已忽略)
│           ├── windows/     # Windows 平台库文件
│           └── linux/       # Linux 平台库文件
├── .gitignore               # Git 忽略配置
└── README.md                # 项目说明文档
```

## 环境要求

### Windows 平台
- Qt 5.x 或 Qt 6.x
- Visual Studio 2019 或更高版本(用于 MSVC 编译器)
- DirectX 11 SDK(通常随 Windows SDK 一起安装)
- FFmpeg(可选,用于视频编码模式)

### Linux 平台
- Qt 5.x 或 Qt 6.x
- GCC 编译器
- X11 开发库: `libx11-dev`, `libxtst-dev`, `libxdamage-dev`, `libxcomposite-dev`, `libxrender-dev`
- FFmpeg(可选,用于视频编码模式)

## 编译说明

### Windows 平台

1. 安装 Qt(推荐使用 Qt Online Installer)
2. 打开 Qt Creator,打开 `src/qtremotedesktop.pro`
3. 配置构建套件(选择 MSVC 编译器)
4. 点击构建

或使用命令行编译:

```bash
# 设置 Qt 环境
set PATH=C:\Qt\你的Qt版本\msvc版本\bin;%PATH%

# 进入 src 目录
cd src

# 使用 qmake 生成 Makefile
qmake qtremotedesktop.pro

# 编译
nmake
```

### Linux 平台

1. 安装依赖:

```bash
# Ubuntu/Debian
sudo apt-get install qt5-default qtbase5-dev qtwebsockets5-dev \
    libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev

# Fedora/RHEL
sudo dnf install qt5-qtbase-devel qt5-qtwebsockets-devel \
    libX11-devel libXtst-devel libXdamage-devel libXcomposite-devel libXrender-devel
```

2. 编译:

```bash
cd src
qmake qtremotedesktop.pro
make
```

### 启用 FFmpeg 视频编码

要启用 FFmpeg 视频编码功能,需要:

1. 下载 FFmpeg 预编译库并放置到 `src/thridparty/ffmpeg/` 目录
   - Windows: 解压到 `windows/` 子目录
   - Linux: 解压到 `linux/` 子目录

2. 在 `qtremotedesktop.pro` 中取消注释:

```qmake
DEFINES += USE_FFMPEG
```

3. 重新编译项目

FFmpeg 库文件结构:

```
src/thridparty/ffmpeg/
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
bin/qtremotedesktop.exe

# Linux
./bin/qtremotedesktop
```

### Windows 服务模式

```bash
# 安装服务
bin/qtremotedesktop.exe -install

# 启动服务
net start QtRemoteDesktop

# 停止服务
net stop QtRemoteDesktop

# 卸载服务
bin/qtremotedesktop.exe -uninstall
```

默认端口:
- WebSocket 服务: 8084
- HTTP 服务: 自动分配可用端口

## 使用说明

1. 启动服务器程序
2. 打开浏览器访问 HTTP 服务地址(控制台会显示)
3. 或使用 WebSocket 客户端连接到 `wss://服务器地址:8084`
4. 输入用户名和密码进行认证(默认: admin/admin)
5. 在网页或客户端中可以查看远程桌面并控制

### 文件传输

客户端可以通过发送 JSON 消息进行文件上传和下载:

**上传文件**:
```json
{
  "type": "file_upload",
  "filename": "example.txt",
  "size": 1024
}
```

**下载文件**:
```json
{
  "type": "file_download",
  "filename": "/path/to/file.txt"
}
```

## 配置

### SSL 证书

项目使用自签名证书,位于 `src/sslperm/` 目录:
- `cacert.crt` - 证书文件
- `privkey.pem` - 私钥文件

如需使用自己的证书,替换这两个文件即可。

### 用户认证

默认用户名和密码:
- 用户名: `admin`
- 密码: `admin`

如需修改认证凭据,请修改 `AuthManager` 类中的认证逻辑。

### 模式切换

客户端可以通过发送 JSON 消息切换模式:

```json
{
  "type": "mode_change",
  "mode": "video"  // 或 "image"
}
```

- `video`: 视频模式,使用 FFmpeg 编码(需要启用 FFmpeg)
- `image`: 图片模式,使用 JPEG 压缩

## 技术栈

- **Qt**: 跨平台应用程序框架
- **WebSocket**: 实时双向通信协议
- **FFmpeg**: 多媒体处理库(可选)
- **DirectX 11**: Windows 平台屏幕捕获
- **X11**: Linux 平台屏幕捕获
- **Windows API**: Windows 平台输入模拟和服务管理
- **QFile/QIODevice**: 文件传输功能

## 注意事项

1. FFmpeg 二进制文件和 DLL 已被 .gitignore 忽略,需要自行下载
2. 编译输出目录 `bin/` 已被忽略
3. Windows 平台需要 MSVC 编译器
4. Linux 平台需要 X11 相关开发库
5. SSL 证书使用自签名证书,浏览器可能会有安全警告

## 许可证

请查看项目许可证文件。

## 贡献

欢迎提交 Issue 和 Pull Request。