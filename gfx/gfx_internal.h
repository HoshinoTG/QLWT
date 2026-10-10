#pragma once

// ---------------------------------------------------------------------------
// gfx 内部接口：只给 gfx/*.cpp 之间互相调用，游戏代码不要 include
// ---------------------------------------------------------------------------

#include "types.h"

namespace gfxdetail
{
// ---- window.cpp 提供给 batch.cpp 的钩子 -----------------------------------
void beginFrameTiming();       // BeginDrawing：结算上一帧的 update 段耗时
void swapAndEndFrame();        // EndDrawing：提交批次后交换缓冲、计时、限帧、收事件
bool hasWindow();
void framebufferSize(int* width, int* height);

// ---- batch.cpp 初始化（InitWindow 建好 GL 上下文后调用）--------------------
void initBatch();

// ---- batch.cpp 提供给 image.cpp / text.cpp 的钩子 --------------------------
// 1x1 白色贴图：画纯色形状时当纹理用（省一个着色器分支）
Texture2D whiteTexture();
// 把当前批次立即提交（截图 / 切换渲染目标 / 改混合状态前必须调用）
void flushBatch();
// 当前渲染目标的像素尺寸（默认帧缓冲就是帧缓冲尺寸）
int currentRenderTargetWidth();
int currentRenderTargetHeight();
// 当前正在渲染的 FBO（实时玻璃要把它拷出来做折射取样）
unsigned int currentFramebuffer();
// 当前目标是不是多采样（多采样只能用 1:1 + NEAREST 的 blit 读出来）
bool currentTargetMultisampled();
// 当前投影对应的"逻辑尺寸"：绘制坐标 0..w/0..h 映射到的范围
void currentProjectionSize(float* width, float* height);

// ---- 渲染目标状态快照 ------------------------------------------------------
// 实时玻璃抓取时要在"当前正在渲染的目标内部"再开几次离屏 pass（预模糊），
// 而 BeginTextureMode/EndTextureMode 会把当前目标/投影/视口全部改掉。
// 所以抓取前存一份、抓完恢复，调用方完全感觉不到（否则整个场景会画到屏幕上去）。
struct TargetState
{
    unsigned int fbo = 0;
    unsigned int resolveFbo = 0;
    int width = 0;
    int height = 0;
    float projW = 1.0f;
    float projH = 1.0f;
    Matrix projection{};
    Matrix transform{};
    bool scissor = false;
    int scissorX = 0, scissorY = 0, scissorW = 0, scissorH = 0;
};

void saveTargetState(TargetState* out);
void restoreTargetState(const TargetState& state);

// ---- 实时玻璃的"分层"支持 --------------------------------------------------
// 玻璃抓取的折射源是"当前已经画好的画面"。如果一帧里只有一次抓取，那么画在
// 玻璃面板**里面**的玻璃（按钮等）会折射到"面板背后的背景"，看起来像面板破了个洞。
// 所以：渲染器在画玻璃期间打上标记，批次里每提交一次**非玻璃**绘制就 +1；
// 渲染器发现计数变过就重新抓一次 —— 于是每一层玻璃折射到的都是它自己背后那一层。
void setGlassDrawInProgress(bool inProgress);
unsigned int nonGlassDrawCount();
// 渲染器正在画玻璃期间为 true：这期间提交的批次不算"改变了画面"
extern bool gGlassDrawing;

// ---- 精灵批渲染的底层入口（batch.cpp 内部实现，text.cpp 画字形用）----------
// 以「纹理坐标 + 顶点色」提交一个四边形，坐标为**当前变换下**的绘制坐标
void pushQuad(Texture2D texture, float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1, Color tint);
}
