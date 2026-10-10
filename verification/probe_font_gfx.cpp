// 用 gfx 生成同样"小码点集"的字体图集并导出，和 raylib 的图集对比
#include "gfx/gfx.h"
#include <cstdio>
int main(int argc, char** argv) {
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(64, 64, "probe");
    if (!IsWindowReady()) { printf("no window\n"); return 1; }
    int cps[] = {0x20, 0x41, 0x4E2D, 0x6587, 0x7F6E, 0x8BBE};
    Font f = LoadFontEx(argc > 2 ? argv[2] : "assets/fonts/NotoSansSC-Medium.ttf", 72, cps, 6);
    std::printf("glyphs=%d baseSize=%d padding=%d atlas=%dx%d\n", f.glyphCount, f.baseSize,
                f.glyphPadding, f.texture.width, f.texture.height);
    for (int i = 0; i < f.glyphCount; i++)
        std::printf("  glyph %d value=%d adv=%d off=(%d,%d) rec=(%.0f,%.0f,%.0f,%.0f)\n", i,
                    f.glyphs[i].value, f.glyphs[i].advanceX, f.glyphs[i].offsetX,
                    f.glyphs[i].offsetY, f.recs[i].x, f.recs[i].y, f.recs[i].width,
                    f.recs[i].height);
    UnloadFont(f);
    CloseWindow();
    return 0;
}
