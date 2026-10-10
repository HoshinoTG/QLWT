#!/usr/bin/env bash
# Linux / macOS 构建脚本（与 build.ps1 等价，Makefile 也能用）。
# 依赖：g++(C++20)、GLFW3、系统 OpenGL；音频由自带的 miniaudio 提供，
#       图片 / 字体由自带的 stb 提供，都不需要额外装库。
#   Debian/Ubuntu: sudo apt install g++ libglfw3-dev libgl1-mesa-dev
#   Arch:          sudo pacman -S gcc glfw mesa
#   macOS:         brew install glfw
#
# 产物默认放在仓库根目录：引擎启动时会把工作目录切到可执行文件所在目录，
# 素材是按 assets/... 相对路径找的，所以二进制要和 assets/ 放在同一层。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

out="${1:-qlwt}"

mapfile -t src < <(find core game renderer gfx -name '*.cpp' | sort)
src+=(main.cpp)

if [ "$(uname -s)" = "Darwin" ]; then
    libs=($(pkg-config --libs glfw3 2>/dev/null || echo "-lglfw -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo"))
else
    libs=($(pkg-config --libs glfw3 gl 2>/dev/null || echo "-lglfw -lGL") -lpthread -ldl -lm)
fi

echo "compiling ${#src[@]} sources -> $out"
g++ -std=c++20 -O2 -Wall -Wextra -I. "${src[@]}" -o "$out" \
    $(pkg-config --cflags glfw3 2>/dev/null) "${libs[@]}"
echo "build OK -> $out"
