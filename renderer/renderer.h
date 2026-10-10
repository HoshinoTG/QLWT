#pragma once

#include "gfx/gfx.h"

#include <string>

// UI 绘制辅助：现代扁平风格的圆角面板 / 按钮 / 滑条 / 文字
namespace renderer
{
// 全项目统一圆角：按钮与面板保持同一圆心角
constexpr float kCornerRadius = 12.0f;

Color hexToColor(unsigned int rgba);   // 0xRRGGBB 或 0xAARRGGBB

// ---- 全局主题（由设置界面驱动）-------------------------------------------
// 主题色 / 圆角半径 / 按钮样式对所有控件生效；控件若显式传入 accent 则以显式值为准。
void setTheme(Color accent, float cornerRadiusPx, int buttonStyle);
Color accent();
float cornerRadius();
int buttonStyle();

// 背景高斯模糊（带进程级缓存）。
// 章节选择这类场景会被反复创建/销毁，如果每次构造都重算模糊，进入界面时会卡一下
// （实测 1280x720 要 70ms）。这里按「路径+半径+缩放」缓存，只算一次。
//   downscale：先缩小再模糊，可以大幅省时间（模糊后再放大几乎看不出差别）
Texture2D blurredImage(const std::string& path, int blurSize, float downscale = 0.5f);

// 载入贴图并设置平滑采样（mipmap + 三线性）。
// LoadTexture 默认是最近邻，只要贴图不是 1:1 绘制就会出锯齿——
// 角色立绘（480x900 → 缩到约 0.7 倍）、章节封面、标题背景都走这个函数。
// 设 QLWT_NO_SMOOTH=1 可以退回默认采样，用于对照排查。
Texture2D loadSmoothTexture(const std::string& path);

// ---- 颜色体系 -------------------------------------------------------------
// 面板底色 / 卡片 / 描边 / 文字都从主题色的【色相】推导（明度、饱和度沿用固定档位），
// 这样换成粉、绿、橙任何主题色，整块 UI 都是一个色系，不会出现"蓝面板配粉按钮"。
struct Palette
{
    Color accent;         // 主题色
    Color surface;        // 面板底
    Color surfaceAlt;     // 卡片 / 列表行底
    Color surfaceHot;     // 悬停底
    Color surfaceSunken;  // 禁用 / 凹陷
    Color line;           // 常规描边、分隔线
    Color lineStrong;     // 强调描边
    Color text;           // 主文字
    Color textDim;        // 次要文字
    Color textMuted;      // 禁用文字
    Color highlight;      // "新内容 / 下一章" 提示色（语义色，全局统一）
    Color danger;         // 错误
};
const Palette& palette();

// 颜色混合（t=0 取 a，t=1 取 b）与取色相，供各界面统一调色
Color mix(Color a, Color b, float t);
float hueOf(Color c);

// 每帧开始调用一次：记录本帧 dt（已钳制到合理区间）。
// 控件动画统一从这里取 dt，避免"在 draw 里各取各的帧时间"导致的抖动/闪烁。
void beginFrame(float dt);
float frameDelta();

// 帧率无关的指数趋近：单调收敛、不会来回振荡（悬停闪烁的根因之一就是
// 线性步进在目标附近反复跨越）。
float approach(float current, float target, float speed, float dt);

// ---- 弹簧 ---------------------------------------------------------------
// 面板/抽屉的进出场用弹簧，而不是"线性推进 + 缓动曲线"：弹簧会带一点点过冲
// 再收住，收尾也更快 —— 这是 Apple 那套动效的手感来源。
// 半隐式欧拉积分；stiffness 越大越快，damping 越接近 2*sqrt(stiffness) 越不振荡。
struct Spring
{
    float value = 0.0f;
    float velocity = 0.0f;      // 单位：目标值/秒（对 0..1 的进度来说就是"每秒走完几倍"）
    float stiffness = 320.0f;
    float damping = 26.0f;      // ≈ 0.73 倍临界阻尼：约 4% 过冲

    void reset(float v) { value = v; velocity = 0.0f; }
    void update(float target, float dt);
    // 弹簧算出来的值离目标足够近时会被吸附到目标，方便当作 0..1 的进度用
    bool settled() const { return value == 0.0f || value == 1.0f; }
};

// ---- 形变（"玻璃是软的"）------------------------------------------------
// 按运动速度做"沿运动方向拉伸、垂直方向压扁"，近似保持面积。
// 只拉伸不收缩横轴，避免面板比内容还窄。
struct Deform
{
    float sx = 1.0f;
    float sy = 1.0f;
};

// vx/vy 是设计像素/秒；reference 是"形变拉满"的参考速度
Deform deformFromVelocity(float vx, float vy, float reference = 2600.0f,
                          float maxSquash = 0.045f);
// 以 (anchorX, anchorY) 为不动点缩放矩形（0=左/上，1=右/下，0.5=中心）
Rectangle deformRect(Rectangle rect, Deform d, float anchorX = 0.5f, float anchorY = 0.5f);

// 圆角参数为 0..1 比例，此函数把像素半径换算为比例
float roundness(float radiusPx, Rectangle rect);

// 缓动曲线：smoothstep（两头平滑，先快后慢）
inline float easeInOut(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// 缓出曲线：快速启动，缓慢停止
inline float easeOutCubic(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void drawPanel(Rectangle rect, Color fill, float radius,
               Color border = Color{255, 255, 255, 40}, float borderWidth = 2.0f);

// 内描边：DrawRectangleRoundedLinesEx 把描边画在矩形【外侧】
// （外缘 = rect 再外扩 lineThick），于是控件会比布局大一圈，
// 处在裁剪区里时还会被切掉半边边框。这个包装把描边改成"贴着外缘向内 lineThick 像素"，
// 保证控件的可见尺寸 == 布局尺寸，边框永远完整。
void drawRoundedBorder(Rectangle rect, float radiusPx, float lineThick, Color color);

// ---- 毛玻璃面板 -----------------------------------------------------------
// 引擎每帧（有需要时）把「背景 + 立绘」渲染到一张低分辨率快照上，
// 用 setBlurBackdrop 交给渲染器；对话框这类面板就用它做高斯模糊。
//
//   texelsPerPixel：快照纹理像素 / 设计像素 的比例（快照按 1/2 设计分辨率渲染时为 0.5）
void setBlurBackdrop(Texture2D backdrop, float texelsPerPixel);
bool blurBackdropReady();

// 画一块毛玻璃：rect 内部是「背后场景的高斯模糊 + tint 叠加色」，圆角与
// drawPanel 一致；调用方自己再画边框即可（边框保持原有样式不变）。
//   tint.a 越大颜色越实（0 = 纯模糊，1 = 完全被颜色盖住）
//   blurSamples：单边采样数（3 -> 7x7 次采样），blurStep：采样间隔（设计像素）
// 告诉渲染器"设计分辨率"（场景绘制坐标的范围）。实时玻璃的抓取尺寸按它算 ——
// 不能用渲染目标的像素宽：HiDPI 下 RT 是设计尺寸的 1.5 倍，比例算错会让玻璃
// 采到偏移/放大的画面（面板在平滑背景上看不出来，小按钮上就会暴露）。
void setDesignSize(float width, float height);

// 液态玻璃面板：bevelPx = 边缘斜面宽度；border/borderWidth 给一条看得见的描边
void drawBlurPanel(Rectangle rect, Color tint, float radiusPx, float bevelPx = 15.0f,
                   Color border = Color{0, 0, 0, 0}, float borderWidth = 0.0f,
                   bool allowRecapture = true);

// 按钮：圆角矩形，hover 时平滑过渡变色（hoverAnim 保存动画进度 0..1）
// accent 传透明色表示"使用当前主题色"
bool drawButton(Rectangle rect, const std::string& label,
                const Font& font, float fontSize,
                float& hoverAnim, bool hovered, bool pressed, bool enabled = true,
                Color accent = Color{0, 0, 0, 0}, float alpha = 1.0f);

// 滑条：返回新值；drag 表示鼠标是否按住该滑条；accent 传透明色 = 主题色
float drawSlider(Rectangle rect, float value01, bool drag,
                 Color accent = Color{0, 0, 0, 0});

// 开关：返回新状态；accent 传透明色 = 主题色
bool drawToggle(Rectangle rect, bool on, bool clicked, Color accent = Color{0, 0, 0, 0});

// 卡片高光扫过：一条斜向光带（progress 从 -0.35 到 1.35 扫一遍，加色混合）
// 用在"下一章"这类需要吸引注意的卡片上
void drawShine(Rectangle rect, float radiusPx, float progress, float strength = 0.5f);

// 顶部细高亮线（现代面板装饰）
void drawAccentLine(Rectangle rect, Color color, float width = 3.0f);

// 平滑渐变：用多段细条插值绘制，避免 8 位色阶断层（条带化）
void drawGradientH(Rectangle rect, Color c1, Color c2, int strips = 64);
void drawGradientV(Rectangle rect, Color c1, Color c2, int strips = 64);
}
