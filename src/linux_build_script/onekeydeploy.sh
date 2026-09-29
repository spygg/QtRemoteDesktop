#!/bin/bash

# ==================== 功能1：检查参数并显示用法提示 ====================
if [ $# -ne 1 ]; then
    echo "用法: $0 <可执行文件>"
    echo "示例: $0 demo"
    exit 1
fi

# ==================== 功能2：检测 locate 工具 ====================
if ! command -v locate &>/dev/null; then
    echo "错误：未找到 locate 命令，请先安装 mlocate 或类似工具。"
    echo "Ubuntu/Debian: sudo apt install mlocate"
    echo "CentOS/RHEL:   sudo yum install mlocate"
    echo "更新数据库：   sudo updatedb"
    exit 1
fi

# ==================== 功能2.1：检测打包所需的基础工具 ====================
# ldd 来自 glibc（Debian 系为 libc-bin，RHEL 系为 glibc-common），
# realpath/readlink/cp 来自 coreutils。缺失时给出可直接复制的安装命令。
for t in ldd cp; do
    if ! command -v "$t" &>/dev/null; then
        echo "错误：未找到 $t 命令。"
        echo "Ubuntu/Debian: sudo apt install coreutils libc-bin"
        echo "CentOS/RHEL:   sudo yum install coreutils glibc-common"
        echo "Arch:          sudo pacman -S coreutils glibc"
        exit 1
    fi
done
if ! command -v realpath &>/dev/null && ! command -v readlink &>/dev/null; then
    echo "错误：未找到 realpath 或 readlink 命令。"
    echo "Ubuntu/Debian: sudo apt install coreutils"
    echo "CentOS/RHEL:   sudo yum install coreutils"
    echo "Arch:          sudo pacman -S coreutils"
    exit 1
fi

qxcbs=`locate libqxcb.so`
numOfqxcbs=`locate libqxcb.so | wc -l`

# 处理没有找到 libqxcb.so 的情况
if [ -z "$qxcbs" ]; then
    echo "错误：未找到 libqxcb.so，请确保已安装 Qt 平台插件或更新 locate 数据库（sudo updatedb）"
    exit 1
fi

if [ $numOfqxcbs -ne 1 ]; then
    cindex=0
    for xcb in $qxcbs
    do  
        ((cindex++))
        echo $cindex $xcb	
    done

    echo ""
    echo "choose the serial of libqxcb.so :" 
    read serial
else
    # 只有一个结果时自动选择序号1
    serial=1
fi

cindex=0
for i in $qxcbs
do  
    qcxb=$i
    ((cindex++))
    if [ ${cindex} -eq ${serial} ]; then
        break
    fi	
done

echo "libqxcb is:" $qcxb 
echo ""

LibDir=$PWD"/libs"
if [ ! -d "$LibDir" ]; then
    mkdir "$LibDir"
fi

Target=$1
lib_array=($(ldd $Target | grep -o "/.*" | grep -o "/.*/[^[:space:]]*"))

echo "extra Qt lib is: $lib_array"

sql="Sql"
needSql=0
for Variable in ${lib_array[@]}
do
    cp "$Variable" "$LibDir"
    if [[ $Variable == *$sql* ]]
    then
        needSql=1
    fi
done

if [ ${needSql} -eq 1 ]; then
    echo "need sql ${needSql}"
fi

echo "copy $1 libs finshed"
echo "========================"

xcb_array=($(ldd $qcxb | grep -o "/.*" | grep -o "/.*/[^[:space:]]*"))
for Variable in ${xcb_array[@]}
do
    cp "$Variable" "$LibDir"
done

echo " "
echo "copy all  libs of '$qcxb'  finshed"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>"

platforms="$(dirname "$qcxb")"
echo "paltforms dir is : $platforms"

cp -r $platforms  "$PWD"

if  [ "$needSql" -eq 1 ];
then
    sqldrivers="$(dirname "$platforms")/sqldrivers"
    cp -r $sqldrivers "$PWD"
    echo " "
    echo "copy $sqldrivers finshed"
    echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>"
fi

redundancyLib=$PWD/platforms/libs
rm -rf "$redundancyLib"

# ==================== 生成启动脚本 ====================
startup_script="$1.sh"
cat > "$startup_script" << 'EOF'
#!/bin/sh

appname=`basename $0 | sed s,\.sh$,,`
dirname=`dirname $0`

tmp="${dirname#?}"
if [ "${dirname%$tmp}" != "/" ]; then
    dirname=$PWD/$dirname
fi

# ---- OpenSSL 版本对齐（智能检测，不硬绑版本）----
# Qt 在运行期按「可执行文件 NEEDED 的 libssl SONAME」解析 OpenSSL。若宿主系统的 OpenSSL
# 主版本与编译时不一致（如 Qt5.9 需要 1.0.x，而宿主只有 1.1/3.x），TLS 会静默失效。
# 这里从可执行文件自身读出所需的 SONAME，只在候选目录中「确实同时提供 libssl 与
# libcrypto 同版本」的目录上注入；找不到就保持原样，避免破坏其它发行版/环境下的正常场景。
REQ_SSL=""
if command -v readelf >/dev/null 2>&1; then
    REQ_SSL=`readelf -d "$dirname/$appname" 2>/dev/null \
        | sed -n 's/.*NEEDED.*\[\(libssl\.so\.[0-9][0-9.]*\)\].*/\1/p' | sort -u`
fi
if [ -z "$REQ_SSL" ] && command -v objdump >/dev/null 2>&1; then
    REQ_SSL=`objdump -p "$dirname/$appname" 2>/dev/null \
        | sed -n 's/.*NEEDED[[:space:]]*\(libssl\.so\.[0-9][0-9.]*\).*/\1/p' | sort -u`
fi

VERS=""
for s in $REQ_SSL; do
    VERS="$VERS ${s#libssl.so.}"
done
if [ -z "$VERS" ]; then
    VERS=" 1.0.0 1.1 3"
fi
VERS=`echo $VERS`

# 候选目录（优先随包 libs；QTRD_OPENSSL_DIR 可显式覆盖；开发树 build_output 兜底）
cand="$dirname/libs"
if [ -n "$QTRD_OPENSSL_DIR" ]; then
    cand="$QTRD_OPENSSL_DIR:$cand"
fi
cand="$cand:$dirname/../build_output/openssl102_install/lib"
cand=`echo "$cand" | tr ':' ' '`

OSSL_DIR=""
for d in $cand; do
    if [ -z "$d" ] || [ ! -d "$d" ]; then
        continue
    fi
    for v in $VERS; do
        if [ -f "$d/libssl.so.$v" ] && [ -f "$d/libcrypto.so.$v" ]; then
            OSSL_DIR="$d"
            break
        fi
    done
    if [ -n "$OSSL_DIR" ]; then
        break
    fi
done

if [ -n "$OSSL_DIR" ]; then
    LD_LIBRARY_PATH="$OSSL_DIR:$dirname/libs:$LD_LIBRARY_PATH"
    echo "[QtRemoteDesktop] OpenSSL: $OSSL_DIR (libssl.so.$VERS)" >&2
else
    LD_LIBRARY_PATH="$dirname/libs:$LD_LIBRARY_PATH"
    echo "[QtRemoteDesktop] WARN: 未找到与可执行文件匹配的 OpenSSL (libssl.so.$VERS)，TLS 可能不可用" >&2
fi
export LD_LIBRARY_PATH
cd "$dirname/"

export QT_LOGGING_RULES="*.debug=true"

"$dirname/$appname" "$@"
EOF

chmod +x "$startup_script"

echo ""
echo "#################################################"
echo " generate '${startup_script}'  start up script finshed"

# ==================== 功能3：在桌面创建 .desktop 文件 ====================
DESKTOP_DIR="$HOME/Desktop"
# 如果桌面目录不存在，尝试创建（某些系统可能没有，如服务器版）
if [ ! -d "$DESKTOP_DIR" ]; then
    echo "警告：未找到桌面目录 $DESKTOP_DIR，将使用当前目录创建快捷方式"
    DESKTOP_DIR="$PWD"
fi

# 获取可执行文件的基本名称（去掉路径）
base_name=$(basename "$1")
desktop_file="$DESKTOP_DIR/${base_name}.desktop"

# 获取启动脚本的绝对路径
abs_startup_script=$(realpath "$startup_script" 2>/dev/null || readlink -f "$startup_script" 2>/dev/null)
if [ -z "$abs_startup_script" ]; then
    # 如果 realpath/readlink 不可用，使用基于当前目录的绝对路径
    abs_startup_script="$PWD/$startup_script"
fi

# 创建 .desktop 文件
cat > "$desktop_file" << EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=$base_name
Comment=Qt application packaged by onekey.sh
Exec=$abs_startup_script
Icon=
Terminal=false
Categories=Qt;Development;
StartupNotify=true
EOF

# 使 .desktop 文件可执行（部分桌面环境需要）
chmod +x "$desktop_file"

echo "已在桌面创建快捷方式：$desktop_file"
echo ""
echo "#################################################"
echo "所有操作完成！"