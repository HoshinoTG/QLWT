#pragma once

#include "types.h"

// ---------------------------------------------------------------------------
// 屏幕后处理 + 场景转场
//
// 引擎把整个场景画进离屏渲染目标，最后再贴到窗口上；这个模块就插在
// 「场景画完」和「贴到窗口」之间：
//
//   场景 RT ──①亮部提取─→ 1/4 分辨率缓冲 ──②横向模糊──③纵向模糊─→ 泛光图
//          ←────────── ④泛光按加色叠回场景 RT ──────────
//   场景 RT ──⑤合成（转场遮罩 + 暗角 + 调色 + 胶片颗粒）──→ 窗口
//
// 泛光只在 1/4 分辨率上算（软件渲染也扛得住），合成是全屏一次采样，很便宜。
// 质量档为「关」时整个模块不介入，走原来的直接贴图路径（像素与替换前一致）。
// ---------------------------------------------------------------------------

namespace postfx
{
// 特效质量档（设置里可选）
enum Quality
{
    QualityOff = 0,    // 关：直接贴图，什么都不做
    QualityLow = 1,    // 低：1/4 分辨率泛光，1 次模糊
    QualityHigh = 2,   // 高：1/4 分辨率泛光，2 次模糊（光晕更大更柔）
};

// 全局抗锯齿
enum AntiAliasing
{
    AaOff = 0,     // 关（和替换 raylib 之前逐像素一致）
    AaFxaa = 1,    // 后处理 FXAA：一次全屏 pass，便宜，整幅画面都管
    AaMsaa = 2,    // 离屏 4x 多重采样：几何边缘真正的覆盖采样，由渲染目标负责
};

// 转场方式
enum Transition
{
    TransitionNone = 0,
    TransitionFade,       // 黑场淡入淡出
    TransitionDissolve,   // 噪点溶解
    TransitionWipe,       // 以某个点为中心的径向擦除
    TransitionFlash,      // 白闪（中间最亮，配泛光很有"曝光"感）
};

struct Frame
{
    int quality = QualityLow;
    int aa = AaOff;           // 只影响后处理；MSAA 由渲染目标决定
    int transition = TransitionNone;
    float progress = 0.0f;    // 0..1：<0.5 旧画面退场，>=0.5 新画面入场
    float centerX = 0.5f;     // 转场中心（画布 uv，左上为原点）
    float centerY = 0.5f;
    float aspect = 16.0f / 9.0f;
    float flash = 0.0f;       // 0..1 白闪（CG 揭示、战斗开始这类"曝光"瞬间）
    unsigned int seed = 1;    // 每帧换一个，胶片颗粒才不会静止
};

// 建/重建内部缓冲（尺寸 = 场景渲染目标尺寸）。重复调用同尺寸不会重建。
void init(int width, int height);
void shutdown();

// 把场景贴图经过后处理 + 转场画到当前渲染目标（一般是默认帧缓冲）。
//   scene       : 场景渲染目标的颜色贴图
//   sceneTarget : 同一个渲染目标（泛光要按加色叠回去；传 id=0 就跳过泛光）
//   dest        : 画布在屏幕上的矩形
void composite(Texture2D scene, RenderTexture2D sceneTarget, Rectangle dest, const Frame& frame);

// 当前质量档下泛光的强度/半径等默认值（设置界面也可用来显示说明）
float defaultBloom(int quality);
}   // namespace postfx
