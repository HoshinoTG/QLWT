#!/usr/bin/env bash
# 在 Linux 上交叉编译 Windows 版（x86_64），产出：
#   dist/qlwt-windows-x86_64/        qlwt.exe + assets/ + docs/ + README.md
#   dist/qlwt-windows-x86_64.zip     整包 zip，解压即可玩
#
# 依赖：mingw-w64 工具链、cmake（用来编自带的 GLFW 源码）、python3（打 zip 用）。
# 可用环境变量覆盖：
#   MINGW_PREFIX   mingw 工具链前缀（含 bin/x86_64-w64-mingw32-g++）
#
# 说明：exe 静态链接了 GLFW / libstdc++ / libgcc，目标机器不需要额外 DLL，
# 只要 Win10 以上（UCRT 由系统提供、opengl32.dll 是系统自带）即可。
# MP4 等视频 CG 仍需要 ffmpeg.exe。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# ---- 工具链 ----
MINGW_PREFIX="${MINGW_PREFIX:-/home/hoshino/project/.tools/mingw-root/usr}"
if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    export PATH="$MINGW_PREFIX/bin:$PATH"
fi
if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "找不到 x86_64-w64-mingw32-g++：请安装 mingw-w64，或用 MINGW_PREFIX 指定前缀" >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "找不到 cmake：交叉编译 GLFW 需要它（也可以预先把 libglfw3.a 放到 build/glfw-win/src/）" >&2
    exit 1
fi

GLFW_SRC=third_party/glfw
GLFW_BUILD=build/glfw-win
GLFW_LIB="$GLFW_BUILD/src/libglfw3.a"

# ---- GLFW（Windows 静态库）----
if [ ! -f "$GLFW_LIB" ]; then
    if [ ! -f "$GLFW_SRC/CMakeLists.txt" ]; then
        echo "找不到自带的 GLFW 源码：$GLFW_SRC" >&2
        exit 1
    fi
    echo "== 1/3 交叉编译 GLFW（Windows 静态库，源码 $GLFW_SRC）=="
    cmake -S "$GLFW_SRC" -B "$GLFW_BUILD" \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
        -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DGLFW_BUILD_EXAMPLES=OFF \
        -DGLFW_BUILD_TESTS=OFF \
        -DGLFW_BUILD_DOCS=OFF \
        -DGLFW_INSTALL=OFF >/dev/null
    cmake --build "$GLFW_BUILD" -j"$(nproc)" >/dev/null
else
    echo "== 1/3 复用已编好的 GLFW：$GLFW_LIB =="
fi

# 让编译器找到 GLFW 头文件（gfx/window.cpp 里 include <GLFW/glfw3.h>）
GLFW_INC="$GLFW_SRC/include"

out=dist/qlwt-windows-x86_64
echo "== 2/3 交叉编译 qlwt.exe =="
rm -rf "$out"
mkdir -p "$out"
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -I. -I"$GLFW_INC" \
    core/*.cpp game/*.cpp renderer/*.cpp gfx/*.cpp main.cpp \
    -o "$out/qlwt.exe" \
    -L"$GLFW_BUILD/src" -lglfw3 \
    -lopengl32 -lgdi32 -lwinmm -lole32 -luser32 -lshell32 \
    -static -static-libgcc -static-libstdc++ -s
cp -a assets "$out/assets"
cp -a docs "$out/docs"
cp README.md "$out/README.md"

echo "== 3/3 打包 zip =="
rm -f "$out.zip"
python3 - "$out" <<'PY'
import os, sys, zipfile

src = sys.argv[1]
base = os.path.dirname(src)
out = src + ".zip"
count = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for root, _dirs, files in os.walk(src):
        for f in sorted(files):
            p = os.path.join(root, f)
            zf.write(p, os.path.relpath(p, base))
            count += 1
print("zip OK -> %s（%d 个文件）" % (out, count))
PY
du -sh "$out" "$out.zip"
