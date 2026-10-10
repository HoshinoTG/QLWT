// 液态玻璃探针：给一块格子/彩条背景加玻璃面板，折射会把背景"掰弯"，一眼能看出来。
// 顺便导出：无折射 vs 有折射、以及色散/高光的近景。
#include "gfx/gfx.h"
#include "../renderer/renderer.h"

#include <cmath>
#include <cstdio>

namespace
{
// 手动造一张"背景快照"，模拟引擎里 renderer::setBlurBackdrop 的输入
RenderTexture2D makeBackdrop(int width, int height)
{
    RenderTexture2D rt = LoadRenderTexture(width, height);
    BeginTextureMode(rt);
    ClearBackground(Color{16, 20, 34, 255});

    // 格子：折射会让直线在玻璃边缘弯掉
    for (int x = 0; x < width; x += 24)
        DrawRectangle(x, 0, 1, height, Color{90, 110, 150, 255});
    for (int y = 0; y < height; y += 24)
        DrawRectangle(0, y, width, 1, Color{90, 110, 150, 255});

    // 几个高饱和色块 + 亮斑：看色散和饱和度增益
    DrawRectangle(width/8, height/4, width/5, height/4, Color{220, 70, 90, 255});
    DrawRectangle(width/2, height/8, width/4, height/3, Color{70, 190, 120, 255});
    DrawRectangle(width/2, height*2/3, width/3, height/4, Color{230, 200, 80, 255});
    DrawCircleV({static_cast<float>(width)*0.78f, static_cast<float>(height)*0.5f},
                static_cast<float>(height)*0.16f, WHITE);
    EndTextureMode();
    return rt;
}
}   // namespace

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "probe_glass.png";
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(980, 780, "glass probe");
    if (!IsWindowReady()) { std::printf("no window\n"); return 1; }

    renderer::setTheme(Color{90, 140, 255, 255}, 12.0f, 0);
    renderer::beginFrame(1.0f / 60.0f);

    // 背景快照按 1/2 分辨率给（和引擎一致）
    RenderTexture2D backdrop = makeBackdrop(490, 390);

    for (int warm = 0; warm < 4; ++warm) { BeginDrawing(); ClearBackground(BLACK); EndDrawing(); }

    BeginDrawing();
    ClearBackground(Color{10, 12, 20, 255});

    // 背景本体（清晰版），玻璃面板盖在上面
    DrawTexturePro(backdrop.texture,
                   {0, 0, 490, -390}, {0, 0, 980, 780}, {0, 0}, 0.0f, WHITE);

    // 关键：不需要再把背景交给 renderer —— drawBlurPanel 会实时抓取
    // "当前已经画好的画面"（也就是上面这行画完的背景）来做折射。

    // 三块玻璃：不同斜面宽度，直接对比折射强度
    for (int i = 0; i < 3; ++i)
    {
        Rectangle panel{60.0f + i * 270.0f, 540.0f, 240.0f, 200.0f};
        renderer::drawBlurPanel(panel, Color{26, 32, 52, 190}, 14.0f, 4.0f + i * 14.0f);
        renderer::drawRoundedBorder(panel, 14.0f, 2.0f, Color{255, 255, 255, 70});
    }

    // ---- 实时玻璃的关键验证 -------------------------------------------------
    // 先画一块不透明面板，再在它上面盖玻璃：玻璃里应该能"看到"那块面板的边缘被掰弯，
    // 这是"折射当前画面"和"折射背景快照"的区别所在。
    DrawRectangleRounded({60, 250, 300, 120}, 0.14f, 16, Color{46, 92, 160, 255});
    DrawRectangleRounded({250, 300, 320, 90}, 0.14f, 16, Color{180, 120, 60, 255});
    {
        Rectangle over{120.0f, 270.0f, 300.0f, 160.0f};
        renderer::drawBlurPanel(over, Color{26, 32, 52, 170}, 16.0f, 18.0f);
        renderer::drawRoundedBorder(over, 16.0f, 2.0f, Color{255, 255, 255, 70});
    }
    // 再叠一块玻璃在刚才那块玻璃上：第二块应该折射到第一块的边缘
    {
        Rectangle over2{330.0f, 330.0f, 220.0f, 140.0f};
        renderer::drawBlurPanel(over2, Color{30, 36, 60, 150}, 16.0f, 20.0f);
        renderer::drawRoundedBorder(over2, 16.0f, 2.0f, Color{255, 200, 120, 90});
    }

    // 玻璃按钮：三种样式都盖在格子上，看能不能透出背后的格子/色块
    for (int st = 0; st < 3; ++st)
    {
        renderer::setTheme(Color{90, 140, 255, 255}, 12.0f, st);
        float hov = 0.0f;
        renderer::drawButton({80.0f + st*270.0f, 40.0f, 230.0f, 70.0f}, "liquid glass",
                             Font{}, 20.0f, hov, false, false);
    }
    renderer::setTheme(Color{90, 140, 255, 255}, 12.0f, 0);

    // 高亮参照：玻璃边缘处格子的弯折在放大图里最明显
    gfxFlushBatch();
    TakeScreenshot(out);
    EndDrawing();

    UnloadRenderTexture(backdrop);
    CloseWindow();
    std::printf("saved %s\n", out);
    return 0;
}
