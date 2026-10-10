# Windows 构建脚本（MSYS2 / mingw-w64）。
# 依赖：g++(C++20)、GLFW3、系统 OpenGL；音频由自带的 miniaudio 提供，
#       图片 / 字体由自带的 stb 提供，都不需要额外装库。
#
#   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-glfw
#
# 产物 qlwt.exe 要放在 assets\ 同一层（和 Linux 下的 qlwt 一样）。
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$src = @(Get-ChildItem -LiteralPath $root -Recurse -Filter *.cpp |
        Where-Object { $_.FullName -notmatch '\\tools\\' -and
                       $_.FullName -notmatch '\\out\\' -and
                       $_.FullName -notmatch '\\build\\' -and
                       $_.FullName -notmatch '\\third_party\\' } |
        ForEach-Object { $_.FullName })
$out = Join-Path $root 'qlwt.exe'

Write-Host "compiling $($src.Count) sources -> $out"
& g++ -std=c++20 -O2 -Wall -Wextra -I"$root" @src -o $out `
    -lglfw3 -lopengl32 -lgdi32 -lwinmm -lole32 -luser32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "build OK"
