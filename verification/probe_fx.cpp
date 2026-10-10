// UI 特效探针：一屏画出「主题色悬停柔光 + 卡片高光扫过」，导出 PNG 检查形状/透明度。
// 编译：见 README（gfx + renderer 两个模块）
#include "gfx/gfx.h"
#include "../renderer/renderer.h"

#include <cstdio>

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "probe_fx.png";
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(900, 520, "fx probe");
    if (!IsWindowReady()) { std::printf("no window\n"); return 1; }

    renderer::setTheme(Color{90, 140, 255, 255}, 12.0f, 0);
    renderer::beginFrame(1.0f / 60.0f);

    // Wayland 下第一帧的帧缓冲还没配置好，先空跑几帧再画（`--selftest` 的自检截图也是这个道理）
    for (int warmup = 0; warmup < 5; ++warmup)
    {
        BeginDrawing();
        ClearBackground(Color{0, 0, 0, 255});
        EndDrawing();
    }

    BeginDrawing();
    ClearBackground(Color{14, 17, 26, 255});

    // 三个按钮：普通 / 悬停中 / 悬停到位（看柔光强度梯度）
    const Font none{};
    float h0 = 0.0f, h1 = 0.5f, h2 = 1.0f, h3 = 1.0f;
    renderer::drawButton({40, 40, 240, 64}, "idle", none, 20.0f, h0, false, false);
    renderer::drawButton({40, 130, 240, 64}, "half", none, 20.0f, h1, true, false);
    renderer::drawButton({40, 220, 240, 64}, "full", none, 20.0f, h2, true, false);
    renderer::drawButton({40, 310, 240, 64}, "press", none, 20.0f, h3, true, true);

    // 三种按钮样式（深色 / 浅色 / 描边）对比
    for (int st = 0; st < 3; ++st)
    {
        renderer::setTheme(Color{90, 140, 255, 255}, 12.0f, st);
        float hh = 0.0f;
        renderer::drawButton({330.0f + st*180.0f, 250, 160, 56}, "style", none, 18.0f, hh, false, false);
    }
    renderer::setTheme(Color{90, 140, 255, 255}, 12.0f, 0);

    // 卡片高光扫过：同一张卡片画四个进度，看光带位置
    for (int i = 0; i < 4; ++i)
    {
        Rectangle card{330.0f + i * 140.0f, 40.0f, 120.0f, 160.0f};
        renderer::drawPanel(card, Color{26, 34, 58, 240}, 12.0f);
        renderer::drawShine(card, 12.0f, -0.35f + i * 0.5f, 0.75f);
    }

    // 面板 + 圆角描边（看 drawPanel / 内描边不溢出）
    renderer::drawPanel({330, 230, 540, 150}, Color{26, 34, 58, 240}, 12.0f);
    renderer::drawRoundedBorder({330, 230, 540, 150}, 12.0f, 2.0f, Color{120, 160, 255, 180});
    for (int i = 0; i < 5; ++i)
        DrawCircleV({380.0f + i * 120.0f, 305.0f}, 10.0f + i * 3.0f, Color{255, 255, 255, 255});

    // 悬停光晕边缘检查：放大后不该出现方形边界
    gfxFlushBatch();
    TakeScreenshot(out);
    EndDrawing();
    CloseWindow();
    std::printf("saved %s\n", out);
    return 0;
}
