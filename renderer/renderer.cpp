#include "renderer.h"

#include "../gfx/gfx_internal.h"
#include "../gfx/gl_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "gfx/gfx.h"

namespace renderer
{
// 当前帧序号：实时玻璃的抓取「一帧只做一次」靠它
unsigned int currentFrameIndex();

namespace
{
// ---- 实时玻璃源 -----------------------------------------------------------
// 定义在文件后半段（captureGlassSource），这里先声明：按钮着色器也要用它，
// 这样"按钮也是真玻璃"和面板走同一条抓取路径。
Texture2D gBackdrop{};          // 雾面底（1/4 分辨率，重模糊过）
float gBackdropScale = 0.5f;
RenderTexture2D gGlassSrc{};    // "锐利"源（1/2 分辨率，没模糊过）：斜面折射用
int gGlassSrcW = 0;
int gGlassSrcH = 0;
float gGlassScale = 0.5f;       // 设计像素 -> 锐利源 uv 的换算基准（按真实贴图宽度算）
float gDesignW = 1280.0f;       // 设计分辨率（场景绘制坐标的范围）
float gDesignH = 720.0f;
bool gGlassReady = false;
bool captureGlassSource(bool allowRecapture = true);

// ---- 毛玻璃：高斯模糊着色器 -----------------------------------------------
// 场景快照（低分辨率）-> 二维高斯采样 -> 叠加 tint -> 按圆角 SDF 裁剪。
// 采样偏移在「快照纹素」单位里算，所以快照分辨率越低模糊越强、开销越小。
const char* kBlurVert = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

const char* kBlurFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 uTexel;      // 1 / 预模糊贴图尺寸
uniform float uStep;      // 采样间隔（保留：需要额外补糊时用）
uniform int uSamples;     // 单边采样数（0 = 只按折射取样三次）
uniform vec2 uUVMin;      // 面板左上角在贴图里的 uv
uniform vec2 uUVSize;     // 面板在贴图里的 uv 尺寸（y 为负：贴图是翻转的）
uniform vec2 uPanelSize;  // 面板尺寸（设计像素）
uniform float uRadius;    // 圆角半径（设计像素）
uniform vec4 uTint;       // 叠加色（a 为混入比例）
uniform vec2 uPxToUV;     // 1 设计像素 = 多少 uv（y 已带翻转符号）
uniform float uBevel;     // 边缘斜面宽度（设计像素）：折射在这一圈里发生
uniform float uRefract;   // 斜面折射强度（设计像素）
uniform float uDisperse;  // 色散：R/B 折射量的差异
uniform float uSpecular;  // 边缘高光强度
uniform float uSheen;     // 顶部内侧薄光
uniform float uSaturate;  // 背后颜色饱和度增益
uniform float uAdaptive;  // 底色跟随背后明暗的程度（0..1）
uniform float uThickness; // 玻璃厚度：边缘内侧压暗
uniform vec2 uLightDir;   // 高光方向（由鼠标位置算出来，默认左上）
uniform sampler2D uGlassSharp;  // 1 号单元：**未重模糊**的原始折射源
uniform vec2 uSharpPxToUV;      // 上者在"设计像素 -> uv"上的换算
uniform float uSharpMix;        // 斜面上用锐利源的比例（0 = 全用雾面底）
uniform float uReflect;         // 环境反射强度
uniform float uMagnify;         // 透镜放大率（面板内背景整体放大，跨轮廓的线条会明显错位）
uniform vec4 uBorder;           // 玻璃边框颜色
uniform float uBorderW;         // 边框宽度（向内）
out vec4 finalColor;

// 圆角矩形距离场：内部为负、边界为 0
float roundedBox(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    // ---- 面板局部坐标 + 距离场 ----
    vec2 uv = (fragTexCoord - uUVMin)/uUVSize;     // 0..1，左上为原点
    vec2 p = (uv - 0.5)*uPanelSize;                // 设计像素，y 向下
    vec2 halfSize = uPanelSize*0.5;
    float r = min(uRadius, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    float d = roundedBox(p, halfSize, r);
    float aa = max(fwidth(d), 0.6);
    float mask = 1.0 - smoothstep(-aa, aa, d);
    float edgeIn = -d;                             // 离边界往内多少像素

    // 边界法线（圆角处也对）
    vec2 n = normalize(max(q, 0.0)*sign(p) + vec2(1e-6));

    // ---- 边缘斜面折射（液态玻璃的灵魂）----
    // 真玻璃的圆角是一圈斜面：贴边处法线偏得最厉害，越往里越平。
    // 用 (1-bevel)^2 当剖面，取样点沿着法线往外偏 -> 背后画面在边缘被"掰弯"、
    // 靠近边缘的内容被压缩，看起来才有厚度。
    // 斜面剖面：**贴边最强、往里快速衰减**。这条很关键 ——
    // 反过来（贴边为 0、往里最大）只会把平滑背景平滑地推一下，肉眼看不出任何折射；
    // 贴边最强才让"内容在轮廓处错位、并被压进边缘那一圈"，这才是玻璃可读的原因。
    float bevelT = clamp(edgeIn/max(uBevel, 0.001), 0.0, 1.0);
    float lens = (1.0 - bevelT)*(1.0 - bevelT);
    vec2 refr = n*(uRefract*lens);

    // ---- 取样 ----
    // 绝大多数像素（斜面以外）折射量是 0，三次取样结果完全一样：
    // 只有真的在斜面附近才走"R/G/B 分别偏移"的色散分支，省下 2/3 的取样。
    // 透镜：以面板中心为原点把采样位置往内收一点 = 面板内背景被放大（跨轮廓的线条会错位）
    // 注意：GLSL 的 const 只能接常量表达式，这里必须是普通变量（踩过两次）
    vec2 centerUV = uUVMin + uUVSize*0.5;
    vec2 baseUV = centerUV + (fragTexCoord - centerUV)*(1.0 - uMagnify);

    vec3 glass;
    if (lens > 0.0015)
    {
        // R/B 的折射量略有不同 -> 边缘一圈极淡的色散
        glass.r = texture(texture0, baseUV + refr*(1.0 + uDisperse)*uPxToUV).r;
        glass.g = texture(texture0, baseUV + refr*uPxToUV).g;
        glass.b = texture(texture0, baseUV + refr*(1.0 - uDisperse)*uPxToUV).b;
    }
    else
    {
        glass = texture(texture0, baseUV).rgb;
    }

    // 需要额外补糊时才走这圈（默认 0：背景已经预模糊过）
    if (uSamples > 0)
    {
        vec3 acc = vec3(0.0);
        float wsum = 0.0;
        const float sigma = 1.8;
        for (int i = -3; i <= 3; ++i)
        {
            if (i < -uSamples || i > uSamples) continue;
            for (int j = -3; j <= 3; ++j)
            {
                if (j < -uSamples || j > uSamples) continue;
                float w = exp(-(float(i*i) + float(j*j))/(2.0*sigma*sigma));
                acc += texture(texture0, fragTexCoord + vec2(float(i), float(j))*uTexel*uStep
                                         + refr*uPxToUV).rgb*w;
                wsum += w;
            }
        }
        glass = mix(glass, acc/wsum, 0.5);
    }

    // ---- 斜面用"锐利"的那份源：重模糊过的底是看不出折射的（背后内容本来就糊成一片），
    //      所以贴边这一圈单独采未模糊版本，位移才看得出来。带宽只有十几像素，开销可忽略 ----
    if (uSharpMix > 0.001 && lens > 0.02)
    {
        vec3 sharp;
        sharp.r = texture(uGlassSharp, baseUV + refr*(1.0 + uDisperse)*uSharpPxToUV).r;
        sharp.g = texture(uGlassSharp, baseUV + refr*uSharpPxToUV).g;
        sharp.b = texture(uGlassSharp, baseUV + refr*(1.0 - uDisperse)*uSharpPxToUV).b;
        glass = mix(glass, sharp, uSharpMix);
    }

    // ---- 颜色：提饱和 + 底色沾一点背后的明暗（自适应）----
    float luma = dot(glass, vec3(0.2126, 0.7152, 0.0722));
    glass = mix(vec3(luma), glass, uSaturate);

    // 自适应用的"背后亮度"直接取刚采到的像素（它就是这一点的背景），
    // 不必再去中心采一次 —— 顺带还变成"局部自适应"，比整块面板一个值更自然。
    float behindLuma = luma;
    vec3 tint = uTint.rgb;
    float tintLuma = dot(tint, vec3(0.2126, 0.7152, 0.0722));
    float targetLuma = mix(tintLuma, behindLuma, uAdaptive);
    tint *= (targetLuma + 0.02)/(tintLuma + 0.02);      // 只改明暗，色相不动

    vec3 col = mix(glass, tint, uTint.a);

    // ---- 玻璃厚度：贴边内侧压一点点暗 ----
    col *= mix(1.0 - uThickness, 1.0, smoothstep(0.0, uBevel*0.5, edgeIn));

    // ---- 反光：左上(或鼠标方向)打光 + 环境反射 + 顶部薄光 ----
    float rimMask = 1.0 - smoothstep(0.0, max(uBevel*0.55, 4.0), edgeIn);
    float ndl = clamp(dot(n, normalize(uLightDir)), 0.0, 1.0);
    float spec = ndl*ndl;   // 原来用 pow(...,2.2)，软件渲染下 pow 太贵，2 次方几乎看不出差别
    float fresnel = (1.0 - ndl)*(1.0 - ndl);      // 掠射角（边缘）反射更强
    col += vec3(uSpecular*spec*rimMask);
    // 环境反射：把雾面源上下翻过来采样 —— 廉价但确实是"反射到周围环境"
    if (uReflect > 0.001)
    {
        // 上下翻转采样 = 廉价但确实"映出周围环境"的反射（跟着 baseUV 一起放大/位移）
        vec3 refl = texture(texture0, vec2(baseUV.x,
                                           uUVMin.y + uUVSize.y - (baseUV.y - uUVMin.y))).rgb;
        col += refl*uReflect*fresnel*rimMask;
    }
    col += vec3(uSheen*(1.0 - smoothstep(0.0, uPanelSize.y*0.22, p.y + halfSize.y))*rimMask);

    // ---- 边框：贴边一圈实色描边，玻璃得有个"轮廓" ----
    float borderA = (1.0 - smoothstep(uBorderW - aa, uBorderW + aa, edgeIn))*uBorder.a;
    col = mix(col, uBorder.rgb, borderA);

    finalColor = vec4(clamp(col, 0.0, 1.0), mask*fragColor.a);
}
)";

// ---- 卡片高光扫过：一条斜向白色光带 -----------------------------------------
// 用加色混合画在卡片上，配合引擎的泛光会有点像"反光扫过"。
const char* kShineVert = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

const char* kShineFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float uProgress;    // -0.35 .. 1.35，光带位置
uniform float uStrength;    // 亮度
uniform vec2 uPanelSize;    // 面板像素尺寸（算圆角遮罩）
uniform float uRadius;      // 圆角半径（像素）
out vec4 finalColor;
void main()
{
    // 圆角遮罩：和 drawPanel 的圆角一致，光带不会溢出圆角
    vec2 p = (fragTexCoord - 0.5)*uPanelSize;
    vec2 halfSize = uPanelSize*0.5;
    float r = min(uRadius, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float aa = max(fwidth(d), 0.7);
    float mask = 1.0 - smoothstep(-aa, aa, d);
    if (mask <= 0.002) discard;

    // 斜向光带（左上 -> 右下）
    float axis = fragTexCoord.x + fragTexCoord.y*0.45;
    float band = exp(-pow((axis - uProgress*1.45)/0.17, 2.0));
    finalColor = vec4(vec3(1.0), band*mask*uStrength*fragColor.a);
}
)";

Shader gBlurShader{};
bool gBlurReady = false;
int gLocTexel = -1, gLocStep = -1, gLocSamples = -1, gLocUVMin = -1,
    gLocUVSize = -1, gLocPanel = -1, gLocRadius = -1, gLocTint = -1;
int gLocPxToUV = -1, gLocRefract = -1, gLocSpecular = -1, gLocSheen = -1, gLocSaturate = -1;
int gLocBevel = -1, gLocDisperse = -1, gLocAdaptive = -1, gLocThickness = -1, gLocLightDir = -1;
int gLocGlassSharp = -1, gLocSharpPxToUV = -1, gLocSharpMix = -1, gLocReflect = -1;
int gLocMagnify = -1;
int gLocBorder = -1, gLocBorderW = -1;
Shader gShineShader{};
bool gShineReady = false;
int gShineProgress = -1, gShineStrength = -1, gShinePanel = -1, gShineRadius = -1;

// ---- 按钮底板 ---------------------------------------------------------------
// 一个着色器把「圆角 + 竖直渐变 + 顶部内高光 + 内描边 + 柔和投影 + 悬停外发光」
// 一次画完，而且圆角是距离场算的，边缘自带抗锯齿（不依赖 MSAA）。
const char* kButtonVert = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

const char* kButtonFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 uPanel;         // 按钮尺寸（设计像素，不含外扩边距）
uniform float uMargin;       // 四边外扩（给投影和发光留的地方）
uniform float uRadius;       // 圆角半径（像素）
uniform vec4 uTop;           // 渐变上端颜色
uniform vec4 uBottom;        // 渐变下端颜色
uniform vec4 uBorder;        // 描边颜色
uniform float uBorderW;      // 描边宽度（向内）
uniform float uRim;          // 顶部内高光强度
uniform float uShade;        // 底部内阴影强度
uniform float uShadow;       // 投影强度
uniform float uShadowSoft;   // 投影模糊半径
uniform float uShadowDrop;   // 投影下移像素
uniform vec4 uGlow;          // 悬停外发光颜色（a = 强度）
uniform float uGlowSpread;   // 发光扩散半径
uniform vec2 uGlassUVMin;    // 折射源（实时抓的当前画面）在按钮四角处的 uv
uniform vec2 uGlassUVSize;
uniform vec2 uGlassPxToUV;   // 1 设计像素 = 多少 uv（y 带翻转符号）
uniform sampler2D uGlassSharpBtn;  // 未重模糊的源（斜面用，才看得出位移）
uniform vec2 uGlassSharpPxToUV;
uniform float uGlassMix;     // 折射到的画面混进底色的比例（0 = 纯色按钮）
uniform float uEdgeFloor;    // 描边强度的下限：保证任何样式都有一条看得见的边
uniform float uGlassBevel;   // 斜面宽度（设计像素）
uniform float uGlassRefract; // 斜面折射强度（设计像素）
uniform vec2 uLightDir;      // 打光方向（Fresnel 高光用）
out vec4 finalColor;

// 圆角矩形距离场：内部为负、边界为 0
float roundedBox(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 quadSize = uPanel + 2.0*uMargin;
    vec2 pos = fragTexCoord*quadSize - uMargin;      // 相对按钮左上角
    vec2 halfSize = uPanel*0.5;
    vec2 p = pos - halfSize;
    float r = min(uRadius, min(halfSize.x, halfSize.y));

    float d = roundedBox(p, halfSize, r);
    float aa = max(fwidth(d), 0.6);
    float inside = 1.0 - smoothstep(-aa, aa, d);
    float edgeIn = max(-d, 0.0);                      // 离边界往内多少像素
    vec2 qb = abs(p) - halfSize + r;
    vec2 grad = normalize(max(qb, 0.0)*sign(p) + vec2(1e-6));   // 边界法线（折射/高光都用）

    // 投影：形状下移 + 外扩，再按距离衰减
    float ds = roundedBox(p - vec2(0.0, uShadowDrop), halfSize + uShadowSoft*0.5, r);
    float shadow = 1.0 - smoothstep(-uShadowSoft, uShadowSoft, ds);

    // 外发光（只在按钮外侧）：到 uGlowSpread 处必须**正好衰减到 0**，
    // 否则会在四边形边界被硬裁成方角（之前 exp() 在边界还剩 37% 强度）
    float glow = 0.0;
    if (uGlowSpread > 0.0)
    {
        float g = clamp(max(d, 0.0)/uGlowSpread, 0.0, 1.0);
        glow = (1.0 - g)*(1.0 - g);
    }

    // 填充：竖直渐变 + 顶部高光 + 底部压暗
    float t = clamp(fragTexCoord.y, 0.0, 1.0);
    vec3 fill = mix(uTop.rgb, uBottom.rgb, t);
    float topness = 1.0 - smoothstep(0.0, halfSize.y*0.9, pos.y);
    float bottomness = smoothstep(halfSize.y*0.65, uPanel.y, pos.y);
    fill += vec3(uRim*topness*(1.0 - smoothstep(0.0, 2.2, edgeIn)));
    fill -= vec3(uShade*bottomness*(1.0 - smoothstep(0.0, 2.6, edgeIn)));
    fill = clamp(fill, 0.0, 1.0);

    // ---- 实时玻璃：折射"当前画面里已经画好的内容"（按钮因此也是真玻璃）----
    float fresnel = 0.0;
    vec3 glassBase = vec3(0.0);
    if (uGlassMix > 0.001)
    {
        // 和玻璃面板一样：贴边最强、往里快速衰减（反过来等于没有折射）
        float bevelT = clamp(edgeIn/max(uGlassBevel, 0.001), 0.0, 1.0);
        float lens = (1.0 - bevelT)*(1.0 - bevelT);
        vec2 refr = grad*(uGlassRefract*lens);
        // fragTexCoord 就是"整张贴图"的 uv（源矩形传的是整张），和面板那条路径一致；
        // 再乘一次 uvMin/uvSize 就会采到别的区域（表现为按钮里透出画面别处的内容）
        vec2 uv = fragTexCoord;
        vec3 glass;
        if (lens > 0.02)
        {
            // 斜面用锐利源 + 色散（R/B 折射量差一点 -> 边缘一圈极淡的彩边）
            glass.r = texture(uGlassSharpBtn, uv + refr*1.16*uGlassSharpPxToUV).r;
            glass.g = texture(uGlassSharpBtn, uv + refr*uGlassSharpPxToUV).g;
            glass.b = texture(uGlassSharpBtn, uv + refr*0.84*uGlassSharpPxToUV).b;
        }
        else
        {
            glass = texture(texture0, uv).rgb;
        }
        // 玻璃模式下的"本体"：折射到的画面当底，按钮自带颜色当染色。
        // 注意不能在原来的半透明底上再混一次背景 —— 那样背景会被算两次，
        // 按钮会淡到几乎看不见（踩过）。这里改成不透明输出，透明度作用在"染色"上。
        glassBase = glass;
        // 边缘按 Fresnel 更亮（掠射角反射更强），做成"玻璃厚度"的那圈高光
        float ndl = clamp(dot(grad, normalize(uLightDir)), 0.0, 1.0);
        fresnel = (1.0 - ndl)*(1.0 - ndl)*(1.0 - ndl);
        glassBase += vec3(uGlassMix*0.6*fresnel*(1.0 - smoothstep(0.0, uGlassBevel*0.45, edgeIn)));
    }

    // 内描边（带强度下限：描边样式的 border 只有白 alpha 40，深色底上根本看不见）
    float borderA = (1.0 - smoothstep(uBorderW - aa, uBorderW + aa, edgeIn))*
                    max(uBorder.a, uEdgeFloor);
    float fillAlpha = mix(uTop.a, uBottom.a, t);
    vec3 body;
    float bodyAlpha;
    if (uGlassMix > 0.001)
    {
        // 折射画面当底 -> 叠按钮染色（透明度作用在染色上）-> 内描边（自带 alpha）。
        // 染色量给一个**下限**：描边样式的 fillAlpha 只有 0.18，直接用会把按钮变成
        // "几乎没有自己的颜色"（整块消失过），下限 0.30 保证它还是一块看得见的玻璃。
        float tintAmount = clamp(fillAlpha, 0.30, 0.72);
        body = mix(glassBase, fill, tintAmount);
        body = mix(body, uBorder.rgb, borderA);
        bodyAlpha = 1.0;
    }
    else
    {
        body = mix(fill, uBorder.rgb, borderA);
        bodyAlpha = fillAlpha;
    }

    // 自下而上合成：投影(黑) -> 发光 -> 本体，最后反预乘交给硬件做常规 alpha 混合
    float sA = shadow*uShadow*(1.0 - inside);
    float gA = glow*uGlow.a*(1.0 - inside);
    float bA = inside*bodyAlpha;

    vec3 acc = vec3(0.0);
    float a = sA;
    acc = uGlow.rgb*gA + acc*(1.0 - gA);
    a = gA + a*(1.0 - gA);
    acc = body*bA + acc*(1.0 - bA);
    a = bA + a*(1.0 - bA);

    finalColor = vec4(a > 0.0005 ? acc/a : vec3(0.0), clamp(a, 0.0, 1.0));
}
)";

Shader gButtonShader{};
bool gButtonReady = false;
int gBtnPanel = -1, gBtnMargin = -1, gBtnRadius = -1, gBtnTop = -1, gBtnBottom = -1;
int gBtnBorder = -1, gBtnBorderW = -1, gBtnRim = -1, gBtnShade = -1;
int gBtnShadow = -1, gBtnShadowSoft = -1, gBtnShadowDrop = -1, gBtnGlow = -1, gBtnGlowSpread = -1;
int gBtnGlassUVMin = -1, gBtnGlassUVSize = -1, gBtnGlassPxToUV = -1, gBtnGlassMix = -1;
int gBtnGlassBevel = -1, gBtnGlassRefract = -1, gBtnLightDir = -1, gBtnEdgeFloor = -1;
int gBtnGlassSharp = -1, gBtnGlassSharpPxToUV = -1;

bool ensureButtonShader()
{
    if (gButtonReady) return gButtonShader.id != 0;
    gButtonShader = LoadShaderFromMemory(kButtonVert, kButtonFrag);
    if (gButtonShader.id == 0) return false;
    gBtnPanel = GetShaderLocation(gButtonShader, "uPanel");
    gBtnMargin = GetShaderLocation(gButtonShader, "uMargin");
    gBtnRadius = GetShaderLocation(gButtonShader, "uRadius");
    gBtnTop = GetShaderLocation(gButtonShader, "uTop");
    gBtnBottom = GetShaderLocation(gButtonShader, "uBottom");
    gBtnBorder = GetShaderLocation(gButtonShader, "uBorder");
    gBtnBorderW = GetShaderLocation(gButtonShader, "uBorderW");
    gBtnRim = GetShaderLocation(gButtonShader, "uRim");
    gBtnShade = GetShaderLocation(gButtonShader, "uShade");
    gBtnShadow = GetShaderLocation(gButtonShader, "uShadow");
    gBtnShadowSoft = GetShaderLocation(gButtonShader, "uShadowSoft");
    gBtnShadowDrop = GetShaderLocation(gButtonShader, "uShadowDrop");
    gBtnGlow = GetShaderLocation(gButtonShader, "uGlow");
    gBtnGlowSpread = GetShaderLocation(gButtonShader, "uGlowSpread");
    gBtnGlassUVMin = GetShaderLocation(gButtonShader, "uGlassUVMin");
    gBtnGlassUVSize = GetShaderLocation(gButtonShader, "uGlassUVSize");
    gBtnGlassPxToUV = GetShaderLocation(gButtonShader, "uGlassPxToUV");
    gBtnGlassMix = GetShaderLocation(gButtonShader, "uGlassMix");
    gBtnGlassBevel = GetShaderLocation(gButtonShader, "uGlassBevel");
    gBtnGlassRefract = GetShaderLocation(gButtonShader, "uGlassRefract");
    gBtnLightDir = GetShaderLocation(gButtonShader, "uLightDir");
    gBtnEdgeFloor = GetShaderLocation(gButtonShader, "uEdgeFloor");
    gBtnGlassSharp = GetShaderLocation(gButtonShader, "uGlassSharpBtn");
    gBtnGlassSharpPxToUV = GetShaderLocation(gButtonShader, "uGlassSharpPxToUV");
    gButtonReady = true;
    return true;
}

// 画一块按钮底板（单次 draw call；每块都要刷一次批次，因为 uniform 是按着色器缓存的）
void drawButtonSurface(Rectangle rect, float radiusPx, Color top, Color bottom,
                       Color border, float borderW, float rim, float shade,
                       float shadow, Color glow, float glowSpread)
{
    if (rect.width <= 1.0f || rect.height <= 1.0f) return;
    if (!ensureButtonShader()) return;

    // ---- 玻璃底：直接复用"面板"那条已经验证过的路径（折射/色散/边框都在里面）----
    // 按钮自己也实现过一套折射采样，结果源矩形、uv、翻转各踩一次坑；面板这条路
    // 已经稳定，就让它来做玻璃，按钮着色器只负责高光/阴影/发光。
    if (blurBackdropReady())
    {
        Color glassTint = mix(top, bottom, 0.4f);
        const int boosted = static_cast<int>(glassTint.a) + 110;
        glassTint.a = static_cast<unsigned char>(boosted > 255 ? 255 : boosted);
        // 按钮不触发重抓：本帧已有抓取就直接用（否则每个按钮都会多一次全屏拷贝）
        drawBlurPanel(rect, glassTint, radiusPx,
                      std::min(18.0f, rect.height*0.5f), border, borderW, false);
    }

    const float panel[2] = {rect.width, rect.height};
    const float top4[4] = {top.r/255.0f, top.g/255.0f, top.b/255.0f, top.a/255.0f};
    const float bottom4[4] = {bottom.r/255.0f, bottom.g/255.0f, bottom.b/255.0f, bottom.a/255.0f};
    const float border4[4] = {border.r/255.0f, border.g/255.0f, border.b/255.0f, border.a/255.0f};
    const float glow4[4] = {glow.r/255.0f, glow.g/255.0f, glow.b/255.0f, glow.a/255.0f};
    const float soft = 7.0f;    // 投影模糊半径（设计像素）
    const float drop = 3.0f;    // 投影下移

    // 实时玻璃源：抓一次当前画面（一帧只抓一次，和面板共用），
    // 有就把它当纹理采样，没有就退回纯色按钮（uGlassMix = 0）。
    Texture2D glassTex = gfxdetail::whiteTexture();
    float glassMix = 0.0f;
    float glassUVMin[2] = {0.0f, 0.0f};
    float glassUVSize[2] = {0.0f, 0.0f};
    float glassPxToUV[2] = {0.0f, 0.0f};
    const float margin = std::max(2.0f, std::max(glowSpread + 2.0f, soft*2.0f + drop + 3.0f));
    const Rectangle quad{rect.x - margin, rect.y - margin,
                         rect.width + 2.0f*margin, rect.height + 2.0f*margin};
    // 玻璃底已经在上面用面板那条路径画过了，这里只叠高光/阴影/发光（uGlassMix 保持 0）
    if (false)
    {
        const float texW = static_cast<float>(gBackdrop.width);
        const float texH = static_cast<float>(gBackdrop.height);
        const float sx = quad.x*gBackdropScale;
        const float sy = quad.y*gBackdropScale;
        const float sw = quad.width*gBackdropScale;
        const float sh = quad.height*gBackdropScale;
        glassTex = gBackdrop;
        glassUVMin[0] = sx/texW;
        glassUVMin[1] = (texH - sy)/texH;
        glassUVSize[0] = sw/texW;
        glassUVSize[1] = -sh/texH;
        glassPxToUV[0] = gBackdropScale/texW;
        glassPxToUV[1] = -gBackdropScale/texH;
        // 描边样式最透、深色/浅色样式收着点，保证文字对比度
        const int style = buttonStyle();
        glassMix = (style == 2) ? 0.55f : (style == 1 ? 0.28f : 0.34f);
    }
    const float glassBevel = std::min(18.0f, std::min(rect.width, rect.height)*0.5f);
    const float glassRefract = glassBevel*0.85f;
    float glassSharpPxToUV[2] = {0.0f, 0.0f};
    if (glassTex.id != gfxdetail::whiteTexture().id)
    {
        glassSharpPxToUV[0] = gGlassScale/static_cast<float>(gGlassSrc.texture.width);
        glassSharpPxToUV[1] = -gGlassScale/static_cast<float>(gGlassSrc.texture.height);
        gfxActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gGlassSrc.texture.id);
        gfxActiveTexture(GL_TEXTURE0);
    }
    const float light2v[2] = {-0.5f, -0.86f};

    gfxdetail::setGlassDrawInProgress(true);
    BeginShaderMode(gButtonShader);
    SetShaderValue(gButtonShader, gBtnGlassUVMin, glassUVMin, SHADER_UNIFORM_VEC2);
    SetShaderValue(gButtonShader, gBtnGlassUVSize, glassUVSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(gButtonShader, gBtnGlassPxToUV, glassPxToUV, SHADER_UNIFORM_VEC2);
    SetShaderValue(gButtonShader, gBtnGlassMix, &glassMix, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnGlassBevel, &glassBevel, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnGlassRefract, &glassRefract, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnLightDir, light2v, SHADER_UNIFORM_VEC2);
    const float edgeFloor = 0.32f;
    SetShaderValue(gButtonShader, gBtnEdgeFloor, &edgeFloor, SHADER_UNIFORM_FLOAT);
    const int sharpUnitBtn = 1;
    SetShaderValue(gButtonShader, gBtnGlassSharp, &sharpUnitBtn, SHADER_UNIFORM_SAMPLER2D);
    SetShaderValue(gButtonShader, gBtnGlassSharpPxToUV, glassSharpPxToUV, SHADER_UNIFORM_VEC2);
    SetShaderValue(gButtonShader, gBtnPanel, panel, SHADER_UNIFORM_VEC2);
    SetShaderValue(gButtonShader, gBtnMargin, &margin, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnRadius, &radiusPx, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnTop, top4, SHADER_UNIFORM_VEC4);
    SetShaderValue(gButtonShader, gBtnBottom, bottom4, SHADER_UNIFORM_VEC4);
    SetShaderValue(gButtonShader, gBtnBorder, border4, SHADER_UNIFORM_VEC4);
    SetShaderValue(gButtonShader, gBtnBorderW, &borderW, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnRim, &rim, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnShade, &shade, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnShadow, &shadow, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnShadowSoft, &soft, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnShadowDrop, &drop, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gButtonShader, gBtnGlow, glow4, SHADER_UNIFORM_VEC4);
    SetShaderValue(gButtonShader, gBtnGlowSpread, &glowSpread, SHADER_UNIFORM_FLOAT);

    // 源矩形必须是**整张贴图**：fragTexCoord 是相对源矩形归一化的，
    // 传 {0,0,1,1} 在白贴图（1x1）上恰好等于 0..1，换成大贴图后就只跨 1 个纹素，
    // 距离场会把整块按钮判成"在形状外"，什么都不画（这个坑找了很久）。
    // 源矩形 = **按钮自己那一块**区域（和面板同一条路子）：
    //   * 必须是这块区域，否则整张画面会被压进按钮里（fragTexCoord 是相对源矩形归一化的）
    //   * 高度取负 + y 用「翻转坐标系下的下边缘」，才能纠正快照 RT 的上下翻转，
    //     否则按钮里会映出上下镜像的内容
    const float glassTexW = static_cast<float>(glassTex.width);
    const float glassTexH = static_cast<float>(glassTex.height);
    const float srcX = quad.x*gBackdropScale;
    const float srcY = quad.y*gBackdropScale;
    const float srcW = quad.width*gBackdropScale;
    const float srcH = quad.height*gBackdropScale;
    const Rectangle src{srcX, glassTexH - srcY - srcH, srcW, -srcH};
    DrawTexturePro(glassTex, src, quad, {0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
    gfxdetail::setGlassDrawInProgress(false);
}

// ---- 背景预模糊 -----------------------------------------------------------
// 场景快照本来是"清晰"的，每块玻璃面板各自做 49 抽头高斯的话，面板一大就吃不消
// （设置抽屉 0.8M 像素实测 +6.6ms/帧）。这里先把快照做一次可分离高斯、降到 1/4
// 分辨率存起来，面板只采样它 —— 玻璃开销从此与面板数量/大小无关。
const char* kPreBlurFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 uDir;          // 一步的 uv 偏移（横向或纵向）
out vec4 finalColor;
void main()
{
    vec3 sum = texture(texture0, fragTexCoord).rgb*0.2270270270;
    sum += (texture(texture0, fragTexCoord + uDir*1.3846153846).rgb +
            texture(texture0, fragTexCoord - uDir*1.3846153846).rgb)*0.3162162162;
    sum += (texture(texture0, fragTexCoord + uDir*3.2307692308).rgb +
            texture(texture0, fragTexCoord - uDir*3.2307692308).rgb)*0.0702702703;
    finalColor = vec4(sum, 1.0);
}
)";

Shader gPreBlurShader{};
int gPreLocDir = -1;
RenderTexture2D gPreBlurA{};
RenderTexture2D gPreBlurB{};
int gPreBlurW = 0;
int gPreBlurH = 0;
bool gPreBlurReady = false;

bool ensurePreBlur(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    if (gPreBlurReady && gPreBlurW == width && gPreBlurH == height) return true;

    if (gPreBlurA.id) UnloadRenderTexture(gPreBlurA);
    if (gPreBlurB.id) UnloadRenderTexture(gPreBlurB);
    if (gPreBlurShader.id == 0)
    {
        gPreBlurShader = LoadShaderFromMemory(kBlurVert, kPreBlurFrag);
        if (gPreBlurShader.id == 0) return false;
        gPreLocDir = GetShaderLocation(gPreBlurShader, "uDir");
    }
    gPreBlurA = LoadRenderTexture(width, height);
    gPreBlurB = LoadRenderTexture(width, height);
    gPreBlurW = width;
    gPreBlurH = height;
    gPreBlurReady = gPreBlurA.id != 0 && gPreBlurB.id != 0;
    return gPreBlurReady;
}

// 把快照模糊 + 降采样到 1/4 分辨率（两次可分离高斯，横向一次纵向一次）
void preBlurBackdrop(Texture2D source)
{
    const int w = std::max(1, source.width/2);
    const int h = std::max(1, source.height/2);
    if (!ensurePreBlur(w, h)) return;

    const float bw = static_cast<float>(w);
    const float bh = static_cast<float>(h);
    const float stepX = 2.2f/bw;
    const float stepY = 2.2f/bh;

    BeginTextureMode(gPreBlurA);
    ClearBackground(BLACK);
    BeginShaderMode(gPreBlurShader);
    {
        const float dir[2] = {stepX, 0.0f};
        SetShaderValue(gPreBlurShader, gPreLocDir, dir, SHADER_UNIFORM_VEC2);
        DrawTexturePro(source,
                       {0, 0, static_cast<float>(source.width), -static_cast<float>(source.height)},
                       {0, 0, bw, bh}, {0, 0}, 0.0f, WHITE);
    }
    EndShaderMode();
    EndTextureMode();

    BeginTextureMode(gPreBlurB);
    ClearBackground(BLACK);
    BeginShaderMode(gPreBlurShader);
    {
        const float dir[2] = {0.0f, stepY};
        SetShaderValue(gPreBlurShader, gPreLocDir, dir, SHADER_UNIFORM_VEC2);
        DrawTexturePro(gPreBlurA.texture, {0, 0, bw, -bh}, {0, 0, bw, bh}, {0, 0}, 0.0f, WHITE);
    }
    EndShaderMode();
    EndTextureMode();
}

// ---- 主题状态 --------------------------------------------------------------
Color gAccent{90, 140, 255, 255};
float gCornerRadius = kCornerRadius;
int gButtonStyle = 0;            // 0=深色 1=浅色 2=描边
float gFrameDelta = 1.0f / 60.0f;
unsigned int gFrameIndex = 0;
unsigned int gGlassFrame = 0xFFFFFFFFu;   // 上次抓玻璃源的帧号
unsigned int gGlassNonGlass = 0xFFFFFFFFu; // 抓取时的"非玻璃绘制计数"（用于按层重抓）

bool ensureBlurShader()
{
    if (gBlurReady) return gBackdrop.id != 0;
    gBlurShader = LoadShaderFromMemory(kBlurVert, kBlurFrag);
    if (gBlurShader.id == 0) return false;
    gLocTexel   = GetShaderLocation(gBlurShader, "uTexel");
    gLocStep    = GetShaderLocation(gBlurShader, "uStep");
    gLocSamples = GetShaderLocation(gBlurShader, "uSamples");
    gLocUVMin   = GetShaderLocation(gBlurShader, "uUVMin");
    gLocUVSize  = GetShaderLocation(gBlurShader, "uUVSize");
    gLocPanel   = GetShaderLocation(gBlurShader, "uPanelSize");
    gLocRadius  = GetShaderLocation(gBlurShader, "uRadius");
    gLocTint    = GetShaderLocation(gBlurShader, "uTint");
    gLocPxToUV  = GetShaderLocation(gBlurShader, "uPxToUV");
    gLocRefract = GetShaderLocation(gBlurShader, "uRefract");
    gLocSpecular = GetShaderLocation(gBlurShader, "uSpecular");
    gLocSheen = GetShaderLocation(gBlurShader, "uSheen");
    gLocSaturate = GetShaderLocation(gBlurShader, "uSaturate");
    gLocBevel = GetShaderLocation(gBlurShader, "uBevel");
    gLocDisperse = GetShaderLocation(gBlurShader, "uDisperse");
    gLocAdaptive = GetShaderLocation(gBlurShader, "uAdaptive");
    gLocThickness = GetShaderLocation(gBlurShader, "uThickness");
    gLocLightDir = GetShaderLocation(gBlurShader, "uLightDir");
    gLocGlassSharp = GetShaderLocation(gBlurShader, "uGlassSharp");
    gLocSharpPxToUV = GetShaderLocation(gBlurShader, "uSharpPxToUV");
    gLocSharpMix = GetShaderLocation(gBlurShader, "uSharpMix");
    gLocReflect = GetShaderLocation(gBlurShader, "uReflect");
    gLocMagnify = GetShaderLocation(gBlurShader, "uMagnify");
    gLocBorder = GetShaderLocation(gBlurShader, "uBorder");
    gLocBorderW = GetShaderLocation(gBlurShader, "uBorderW");
    gBlurReady = true;
    return gBackdrop.id != 0;
}

bool ensureShineShader()
{
    if (gShineReady) return gShineShader.id != 0;
    gShineShader = LoadShaderFromMemory(kShineVert, kShineFrag);
    if (gShineShader.id == 0) return false;
    gShineProgress = GetShaderLocation(gShineShader, "uProgress");
    gShineStrength = GetShaderLocation(gShineShader, "uStrength");
    gShinePanel = GetShaderLocation(gShineShader, "uPanelSize");
    gShineRadius = GetShaderLocation(gShineShader, "uRadius");
    gShineReady = true;
    return true;
}

float roundnessOf(float radiusPx, Rectangle rect)
{
    float half = std::min(rect.width, rect.height) * 0.5f;
    if (half <= 0.0f) return 0.0f;
    return std::min(1.0f, radiusPx / half);
}

Color lerpColor(Color a, Color b, float t)
{
    return Color{
        static_cast<unsigned char>(a.r + (b.r - a.r) * t),
        static_cast<unsigned char>(a.g + (b.g - a.g) * t),
        static_cast<unsigned char>(a.b + (b.b - a.b) * t),
        static_cast<unsigned char>(a.a + (b.a - a.a) * t),
    };
}

// 透明 = 未指定，回退到主题色
Color resolveAccent(Color c) { return c.a == 0 ? gAccent : c; }

Palette gPalette{};
std::map<std::string, Texture2D> gBlurCache;

// HSV -> RGB（h: 0..360, s/v: 0..1），用于按主题色相生成整套暗色面板
Color hsv(float h, float s, float v)
{
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f)      { r = c; g = x; }
    else if (hp < 2.0f) { r = x; g = c; }
    else if (hp < 3.0f) { g = c; b = x; }
    else if (hp < 4.0f) { g = x; b = c; }
    else if (hp < 5.0f) { r = x; b = c; }
    else                { r = c; b = x; }
    float m = v - c;
    auto q = [](float f) { return static_cast<unsigned char>(f * 255.0f + 0.5f); };
    return Color{q(r + m), q(g + m), q(b + m), 255};
}

void rebuildPalette(Color accent)
{
    float h = hueOf(accent);
    float s = 0.0f;
    {
        float mx = std::max({accent.r, accent.g, accent.b}) / 255.0f;
        float mn = std::min({accent.r, accent.g, accent.b}) / 255.0f;
        s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
    }
    // 主题色越灰，面板越中性，避免灰主题色时面板染色发脏
    float k = std::min(1.0f, s / 0.60f);

    gPalette.accent        = accent;
    gPalette.surface       = hsv(h, 0.50f * k, 0.125f);
    gPalette.surfaceAlt    = hsv(h, 0.46f * k, 0.170f);
    gPalette.surfaceHot    = hsv(h, 0.44f * k, 0.290f);
    gPalette.surfaceSunken = hsv(h, 0.50f * k, 0.100f);
    gPalette.surface.a       = 247;
    gPalette.surfaceAlt.a    = 240;
    gPalette.surfaceHot.a    = 246;
    gPalette.surfaceSunken.a = 215;
    gPalette.line          = Color{255, 255, 255, 40};
    gPalette.lineStrong    = mix(accent, Color{255, 255, 255, 255}, 0.35f);
    gPalette.lineStrong.a  = 165;
    gPalette.text          = Color{245, 247, 255, 255};
    gPalette.textDim       = Color{172, 182, 208, 255};
    gPalette.textMuted     = Color{126, 131, 148, 255};
    gPalette.highlight     = Color{255, 205, 90, 255};
    gPalette.danger        = Color{255, 120, 120, 255};
}

}

void setTheme(Color accent, float cornerRadiusPx, int buttonStyle)
{
    gAccent = accent;
    gCornerRadius = cornerRadiusPx < 0.0f ? 0.0f : (cornerRadiusPx > 28.0f ? 28.0f : cornerRadiusPx);
    gButtonStyle = (buttonStyle < 0 || buttonStyle > 2) ? 0 : buttonStyle;
    rebuildPalette(accent);
}

Color accent() { return gAccent; }
float cornerRadius() { return gCornerRadius; }
int buttonStyle() { return gButtonStyle; }
const Palette& palette() { return gPalette; }

Texture2D blurredImage(const std::string& path, int blurSize, float downscale)
{
    if (downscale <= 0.0f || downscale > 1.0f) downscale = 1.0f;
    char key[600];
    std::snprintf(key, sizeof(key), "%s#%d@%.2f", path.c_str(), blurSize, downscale);
    auto it = gBlurCache.find(key);
    if (it != gBlurCache.end()) return it->second;

    Texture2D tex{};
    auto t0 = std::chrono::steady_clock::now();
    Image img = LoadImage(path.c_str());
    if (img.data != nullptr)
    {
        if (downscale < 0.999f)
        {
            int w = std::max(1, static_cast<int>(img.width * downscale + 0.5f));
            int h = std::max(1, static_cast<int>(img.height * downscale + 0.5f));
            ImageResize(&img, w, h);
        }
        // 半径按缩放同步缩减，视觉上的模糊半径保持不变
        int r = std::max(1, static_cast<int>(blurSize * downscale + 0.5f));
        ImageBlurGaussian(&img, r);
        tex = LoadTextureFromImage(img);
        if (tex.id) SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
        UnloadImage(img);
    }
    gBlurCache[key] = tex;
    printf("[renderer] 背景模糊 '%s' 已缓存（半径 %d，缩放 %.2f，首次耗时 %.1fms）\n",
           path.c_str(), blurSize, downscale,
           std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count());
    return tex;
}

Texture2D loadSmoothTexture(const std::string& path)
{
    Texture2D tex = LoadTexture(path.c_str());
    if (tex.id == 0) return tex;
    // 对照排查用：设了 QLWT_NO_SMOOTH 就退回默认的最近邻采样
    if (std::getenv("QLWT_NO_SMOOTH") == nullptr)
    {
        GenTextureMipmaps(&tex);
        SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    }
    return tex;
}

Color mix(Color a, Color b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return Color{static_cast<unsigned char>(a.r + (b.r - a.r) * t),
                 static_cast<unsigned char>(a.g + (b.g - a.g) * t),
                 static_cast<unsigned char>(a.b + (b.b - a.b) * t),
                 static_cast<unsigned char>(a.a + (b.a - a.a) * t)};
}

float hueOf(Color c)
{
    float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    float d = mx - mn;
    if (d <= 0.0001f) return 210.0f;   // 灰阶：给一个中性蓝
    float h;
    if (mx == r)      h = 60.0f * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else              h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0.0f) h += 360.0f;
    return h;
}

unsigned int currentFrameIndex()
{
    return gFrameIndex;
}

void setDesignSize(float width, float height)
{
    if (width > 1.0f && height > 1.0f)
    {
        gDesignW = width;
        gDesignH = height;
    }
}

void beginFrame(float dt)
{
    ++gFrameIndex;
    gGlassFrame = 0xFFFFFFFFu;   // 新的一帧：玻璃源作废，第一次用到时重抓
    gGlassNonGlass = 0xFFFFFFFFu;
    gFrameDelta = dt < 0.0f ? 0.0f : (dt > 0.05f ? 0.05f : dt);
}

float frameDelta() { return gFrameDelta; }

void Spring::update(float target, float dt)
{
    if (dt <= 0.0f) return;
    // 掉帧保护：单步最多按 1/20 秒推进（解析解本身不会炸，只是别让面板"瞬移"）
    if (dt > 1.0f/20.0f) dt = 1.0f/20.0f;

    const float omega = std::sqrt(std::max(stiffness, 0.0001f));   // 无阻尼角频率
    float zeta = damping/(2.0f*omega);                             // 阻尼比
    if (zeta > 0.999f) zeta = 0.999f;                              // 临界阻尼当欠阻尼解

    // 解析解：x(t) = A e^{-ζωt} cos(ωd t) + B e^{-ζωt} sin(ωd t)
    // 比"显式欧拉逐步积分"好在：低帧率下不会因为数值阻尼把过冲吃掉
    // （30fps 时欧拉积分会把 ζ=0.75 的弹簧积成过阻尼，一点回弹都看不到）
    const float x0 = value - target;
    const float wd = omega*std::sqrt(1.0f - zeta*zeta);
    const float decay = std::exp(-zeta*omega*dt);
    const float c = std::cos(wd*dt);
    const float sn = std::sin(wd*dt);
    const float a = x0;
    const float b = (velocity + zeta*omega*x0)/wd;

    value = target + decay*(a*c + b*sn);
    velocity = decay*((-zeta*omega*a + b*wd)*c + (-zeta*omega*b - a*wd)*sn);

    // 收尾吸附：贴到目标就停（也让 x >= 1.0 之类的判定成立）
    if (std::fabs(target - value) < 0.0004f && std::fabs(velocity) < 0.03f)
    {
        value = target;
        velocity = 0.0f;
    }
}

Deform deformFromVelocity(float vx, float vy, float reference, float maxSquash)
{
    Deform d;
    const float speed = std::sqrt(vx*vx + vy*vy);
    if (speed < 1.0f || reference <= 0.0f) return d;

    const float k = std::min(speed/reference, 1.0f)*maxSquash;
    if (std::fabs(vx) >= std::fabs(vy))
    {
        d.sx = 1.0f + k;            // 横向运动：左右拉伸
        d.sy = 1.0f - k*0.6f;       // 上下压扁（近似保持面积）
    }
    else
    {
        d.sy = 1.0f + k;
        d.sx = 1.0f - k*0.6f;
    }
    return d;
}

Rectangle deformRect(Rectangle rect, Deform d, float anchorX, float anchorY)
{
    const float ax = rect.x + rect.width*anchorX;
    const float ay = rect.y + rect.height*anchorY;
    const float w = rect.width*d.sx;
    const float h = rect.height*d.sy;
    return Rectangle{ax - w*anchorX, ay - h*anchorY, w, h};
}

float approach(float current, float target, float speed, float dt)
{
    if (dt <= 0.0f) return current;
    // 一阶低通：v' = target + (v - target) * e^(-speed*dt)
    // 任意帧率下收敛速度一致，且严格单调，不会在目标附近来回跳（闪烁）
    float v = target + (current - target) * std::exp(-speed * dt);
    if (std::fabs(target - v) < 0.0015f) v = target;   // 收敛后吸附，避免无限逼近
    return v;
}

float roundness(float radiusPx, Rectangle rect)
{
    return roundnessOf(radiusPx, rect);
}

namespace
{
// 实时玻璃的"折射源"：把**当前渲染目标里已经画好的内容**拷一份出来。
//
// 这是"真液态玻璃"的关键：以前是让场景额外渲一遍背景快照，玻璃只能折射到
// 背景 + 立绘；现在是抓当前帧的真实画面，于是它能折射**任何先画的东西** ——
// 其他面板、按钮、甚至另一块玻璃，而且不用场景配合（Scene 那边那套
// needsBackdrop / drawBackdropOnly 因此整个删掉了）。
//
// 一帧只抓一次（第一次真要用玻璃的时候抓），剩下的面板都复用这份。
RenderTexture2D gGlassCapture{};   // 多采样目标时的 1:1 中转（多采样不能缩放 blit）
bool captureGlassSource(bool allowRecapture)
{
    const unsigned int frame = currentFrameIndex();
    const unsigned int nonGlass = gfxdetail::nonGlassDrawCount();
    // 同一帧里，只要这之后又画了非玻璃内容，就重抓一次 —— 这样"面板里的按钮"
    // 折射到的是面板本身，而不是面板背后的背景（不然像破了个洞）
    if (gGlassFrame == frame)
    {
        // 已有本帧的抓取：允许重抓时按"非玻璃绘制计数"判断是否需要更新，
        // 不允许时（按钮这类小块）直接用现成的，省掉一次全屏拷贝
        if (!allowRecapture || gGlassNonGlass == nonGlass) return gGlassReady;
    }
    gGlassFrame = frame;
    gGlassNonGlass = nonGlass;
    gGlassReady = false;

    const int rtW = gfxdetail::currentRenderTargetWidth();
    const int rtH = gfxdetail::currentRenderTargetHeight();
    if (rtW <= 0 || rtH <= 0) return false;

    // 抓取尺寸按**设计分辨率**算（不是 RT 像素宽）：玻璃的 uv 换算以设计像素为单位，
    // HiDPI 下 RT 是设计的 1.5 倍，混用会让玻璃采到偏移且被放大的画面
    // （面板在平滑背景上看不出来，小按钮上会立刻暴露）。
    // 取"设计的一半"就够：反正后面还要重模糊，糊一点正是磨砂玻璃要的。
    const int srcW = std::max(1, static_cast<int>(gDesignW*0.5f + 0.5f));
    const int srcH = std::max(1, static_cast<int>(gDesignH*0.5f + 0.5f));
    if (gGlassSrcW != srcW || gGlassSrcH != srcH)
    {
        if (gGlassSrc.id) UnloadRenderTexture(gGlassSrc);
        gGlassSrc = LoadRenderTexture(srcW, srcH);
        gGlassSrcW = srcW;
        gGlassSrcH = srcH;
    }
    if (gGlassSrc.id == 0) return false;

    gfxdetail::flushBatch();      // 先把攒着的绘制提交掉，否则拷到的是空的

    // 关键：抓取过程中会开离屏 pass（预模糊），而 BeginTextureMode/EndTextureMode
    // 会把"当前渲染目标/投影/视口/裁剪"全部改掉。游戏里玻璃是在 rt_ 的渲染过程**内部**
    // 画的，所以这里必须把整套状态存下来、抓完原样恢复，否则后面的场景会画到屏幕上去。
    gfxdetail::TargetState saved{};
    gfxdetail::saveTargetState(&saved);

    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    const unsigned int srcFbo = gfxdetail::currentFramebuffer();
    if (gfxdetail::currentTargetMultisampled())
    {
        // 多采样源只能 1:1 + NEAREST 解析出来，再缩到目标尺寸
        if (gGlassCapture.texture.width != rtW || gGlassCapture.texture.height != rtH)
        {
            if (gGlassCapture.id) UnloadRenderTexture(gGlassCapture);
            gGlassCapture = LoadRenderTexture(rtW, rtH);
        }
        if (gGlassCapture.id == 0) return false;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gGlassCapture.id);
        glBlitFramebuffer(0, 0, rtW, rtH, 0, 0, rtW, rtH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gGlassCapture.id);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gGlassSrc.id);
        glBlitFramebuffer(0, 0, rtW, rtH, 0, 0, srcW, srcH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    else
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gGlassSrc.id);
        glBlitFramebuffer(0, 0, rtW, rtH, 0, 0, srcW, srcH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<unsigned int>(prevRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<unsigned int>(prevDraw));

    // 再重模糊 + 降采样到 1/4（两次可分离高斯），玻璃只需要这个"雾面"版本做折射底
    gfxdetail::restoreTargetState(saved);   // 预模糊把状态改了，先恢复到调用方的目标再继续
    preBlurBackdrop(gGlassSrc.texture);
    gfxdetail::restoreTargetState(saved);
    // 比例一律按「贴图实际宽度 / 设计宽度」算，别用推导值（推错一次整块玻璃就歪）
    gGlassScale = static_cast<float>(gGlassSrc.texture.width)/gDesignW;
    if (gPreBlurReady)
    {
        gBackdrop = gPreBlurB.texture;
        gBackdropScale = static_cast<float>(gBackdrop.width)/gDesignW;
    }
    else
    {
        gBackdrop = gGlassSrc.texture;
        gBackdropScale = gGlassScale;
    }
    gGlassReady = gBackdrop.id != 0;
    return gGlassReady;
}
}   // namespace

bool blurBackdropReady()
{
    // 纯能力查询：有渲染目标就能做实时玻璃。
    // 真正抓取放在 drawBlurPanel 里（一帧一次），抓失败时它会退回不透明面板，不会留个洞。
    return gfxdetail::currentRenderTargetWidth() > 0 && gfxdetail::currentRenderTargetHeight() > 0;
}



void drawBlurPanel(Rectangle rect, Color tint, float radiusPx, float bevelPx,
                   Color border, float borderWidth, bool allowRecapture)
{
    if (rect.width < 2.0f || rect.height < 2.0f) return;
    // 需要折射源时现抓当前帧（一帧只抓一次）；抓不到就退回不透明面板，而不是什么都不画
    if (!captureGlassSource(allowRecapture))
    {
        float r = roundness(radiusPx, rect);
        DrawRectangleRounded(rect, r, 16, tint);
        return;
    }
    if (!ensureBlurShader()) return;

    // 面板在设计空间里的位置 -> 预模糊贴图坐标
    const float texW = static_cast<float>(gBackdrop.width);
    const float texH = static_cast<float>(gBackdrop.height);
    const float srcX = rect.x * gBackdropScale;
    const float srcY = rect.y * gBackdropScale;
    const float srcW = rect.width * gBackdropScale;
    const float srcH = rect.height * gBackdropScale;
    // 渲染目标纹理是上下翻转的（采样时用负高度来纠正）。用负高度时，
    // DrawTexturePro 内部会把 source.y 加上高度，所以这里要传「翻转坐标系下的下边缘」
    // = 纹理高 - 面板上边 - 面板高；传 rect.y 会让取样整体偏到画面另一侧。
    const Rectangle src{srcX, texH - srcY - srcH, srcW, -srcH};

    const float texel[2] = {1.0f / texW, 1.0f / texH};
    // DrawTexturePro 内部实际使用的起点是 src.y + srcH = texH - srcY，
    // uv 在竖直方向递减（尺寸取负），着色器里的局部坐标才是 0..1。
    const float uvMin[2] = {srcX / texW, (texH - srcY) / texH};
    const float uvSize[2] = {srcW / texW, -srcH / texH};
    const float panel[2] = {rect.width, rect.height};
    const float tintv[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f,
                            tint.a / 255.0f};
    // 1 设计像素 = 多少 uv。y 取负：贴图是翻转的，往上偏要减 v。
    const float pxToUV[2] = {gBackdropScale / texW, -gBackdropScale / texH};

    // ---- 液态玻璃参数 ------------------------------------------------------
    // 斜面宽度：折射发生在贴边这一圈里（面板越大，斜面按比例放宽一点，观感更接近厚玻璃）
    const float bevel = std::min(bevelPx, std::min(rect.width, rect.height)*0.35f);
    const float refract = std::min(bevel*0.85f, 20.0f);   // 折射强度（设计像素）
    const float magnify = 0.022f;                         // 透镜放大率（2% 就够看出错位）
    const float disperse = 0.16f;    // R/B 折射量差 16% -> 边缘极淡的彩边
    const float specular = 0.42f;    // 边缘高光（深色面板上要给足才看得见）
    const float sheen = 0.05f;       // 顶部薄光
    const float saturate = 1.12f;    // 背后颜色提饱和
    const float adaptive = 0.15f;    // 底色跟随背后明暗（克制一点，别把主题色冲淡）
    const float thickness = 0.10f;   // 玻璃厚度（边缘内侧压暗）
    const float samples = 0.0f;      // 背景已预模糊，不再叠抽头
    const float step = 1.0f;

    // 斜面用未重模糊的那份源（采 1 号单元）；边框默认给一条看得见的亮边
    const float sharpPxToUV[2] = {gGlassScale / static_cast<float>(gGlassSrc.texture.width),
                                  -gGlassScale / static_cast<float>(gGlassSrc.texture.height)};
    const float sharpMix = (gGlassSrc.id != 0) ? 0.75f : 0.0f;
    const float reflect = 0.10f;
    const float border4[4] = {border.r/255.0f, border.g/255.0f, border.b/255.0f, border.a/255.0f};
    const float borderW = borderWidth;
    if (gGlassSrc.id != 0)
    {
        gfxActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gGlassSrc.texture.id);
        gfxActiveTexture(GL_TEXTURE0);
    }

    // 高光方向：以左上为基准，掺一点"光跟着鼠标走"（指针在面板内的相对位置）
    Vector2 lightDir{-0.5f, -0.86f};
    {
        const Vector2 m = GetMousePosition();
        const float dx = (m.x - (rect.x + rect.width*0.5f))/(rect.width*0.5f + 1.0f);
        const float dy = (m.y - (rect.y + rect.height*0.5f))/(rect.height*0.5f + 1.0f);
        if (dx > -1.6f && dx < 1.6f && dy > -1.6f && dy < 1.6f && (dx != 0.0f || dy != 0.0f))
        {
            lightDir.x += dx*0.45f;
            lightDir.y += dy*0.45f;
        }
        const float len = std::sqrt(lightDir.x*lightDir.x + lightDir.y*lightDir.y);
        if (len > 0.0001f) { lightDir.x /= len; lightDir.y /= len; }
    }

    gfxdetail::setGlassDrawInProgress(true);
    BeginShaderMode(gBlurShader);
    SetShaderValue(gBlurShader, gLocTexel, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocStep, &step, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSamples, &samples, SHADER_UNIFORM_INT);
    SetShaderValue(gBlurShader, gLocUVMin, uvMin, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocUVSize, uvSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocPanel, panel, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocRadius, &radiusPx, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocTint, tintv, SHADER_UNIFORM_VEC4);
    SetShaderValue(gBlurShader, gLocPxToUV, pxToUV, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocBevel, &bevel, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocRefract, &refract, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocDisperse, &disperse, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSpecular, &specular, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSheen, &sheen, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSaturate, &saturate, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocAdaptive, &adaptive, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocThickness, &thickness, SHADER_UNIFORM_FLOAT);
    const float light2[2] = {lightDir.x, lightDir.y};
    SetShaderValue(gBlurShader, gLocLightDir, light2, SHADER_UNIFORM_VEC2);
    const int sharpUnit = 1;
    SetShaderValue(gBlurShader, gLocGlassSharp, &sharpUnit, SHADER_UNIFORM_SAMPLER2D);
    SetShaderValue(gBlurShader, gLocSharpPxToUV, sharpPxToUV, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocSharpMix, &sharpMix, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocReflect, &reflect, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocMagnify, &magnify, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocBorder, border4, SHADER_UNIFORM_VEC4);
    SetShaderValue(gBlurShader, gLocBorderW, &borderW, SHADER_UNIFORM_FLOAT);
    DrawTexturePro(gBackdrop, src, rect, {0, 0}, 0.0f, WHITE);
    EndShaderMode();
    gfxdetail::setGlassDrawInProgress(false);
}

Color hexToColor(unsigned int rgba)
{
    if (rgba > 0xFFFFFF)  // 带 alpha
        return Color{static_cast<unsigned char>((rgba >> 24) & 0xFF),
                     static_cast<unsigned char>((rgba >> 16) & 0xFF),
                     static_cast<unsigned char>((rgba >> 8) & 0xFF),
                     static_cast<unsigned char>(rgba & 0xFF)};
    return Color{static_cast<unsigned char>((rgba >> 16) & 0xFF),
                 static_cast<unsigned char>((rgba >> 8) & 0xFF),
                 static_cast<unsigned char>(rgba & 0xFF),
                 255};
}

void drawPanel(Rectangle rect, Color fill, float radius, Color border, float borderWidth)
{
    // 稍微大一点的面板走和按钮同一套底板上色（竖直渐变 + 顶部高光），
    // 免得整块 UI 显得太平；小胶囊/小色块保持平涂，省下每次切着色器的开销。
    if (ensureButtonShader() && rect.width >= 90.0f && rect.height >= 34.0f)
    {
        Color top = mix(fill, Color{255, 255, 255, fill.a}, 0.06f);
        Color bottom = mix(fill, Color{0, 0, 0, fill.a}, 0.06f);
        drawButtonSurface(rect, radius, top, bottom, border, borderWidth, 0.06f, 0.05f,
                          0.0f, Color{0, 0, 0, 0}, 0.0f);
        return;
    }

    float r = roundnessOf(radius, rect);
    DrawRectangleRounded(rect, r, 16, fill);
    if (border.a > 0)
        drawRoundedBorder(rect, radius, borderWidth, border);
}

void drawRoundedBorder(Rectangle rect, float radiusPx, float lineThick, Color color)
{
    if (lineThick <= 0.0f || color.a == 0) return;
    // 向内收缩半个到一个线宽，使描边的外缘正好落在 rect 上
    float t = std::min(lineThick, std::min(rect.width, rect.height) * 0.5f);
    if (t <= 0.0f || rect.width <= 0.0f || rect.height <= 0.0f) return;
    Rectangle in{rect.x + t, rect.y + t, rect.width - 2.0f * t, rect.height - 2.0f * t};
    float inner = radiusPx - t;
    float half = std::min(in.width, in.height) * 0.5f;
    if (inner < 0.0f) inner = 0.0f;
    if (inner > half) inner = half;
    DrawRectangleRoundedLinesEx(in, roundnessOf(inner, in), 16, t, color);
}

bool drawButton(Rectangle rect, const std::string& label,
                const Font& font, float fontSize,
                float& hoverAnim, bool hovered, bool pressed, bool enabled, Color accent,
                float alpha)
{
    if (!enabled) { hovered = pressed = false; }
    if (alpha <= 0.01f) return false;   // 完全透明：跳过绘制（不可见时无需推进动画）
    if (alpha > 1.0f) alpha = 1.0f;
    accent = resolveAccent(accent);

    // 悬停动画：帧率无关的指数趋近（单调收敛，不会在目标附近抖动）
    float target = (hovered && enabled) ? 1.0f : 0.0f;
    hoverAnim = approach(hoverAnim, target, 14.0f, gFrameDelta);

    float e = easeInOut(hoverAnim);   // 非线性悬停变色

    Color baseNorm, baseHover, textBase;
    switch (gButtonStyle)
    {
    case 1:   // 浅色玻璃：浅底深字（半透明，能透出背后）
        baseNorm = mix(gPalette.surfaceAlt, Color{255, 255, 255, 255}, 0.88f);
        baseHover = mix(gPalette.surfaceHot, Color{255, 255, 255, 255}, 0.93f);
        baseNorm.a = 205;
        baseHover.a = 225;
        textBase = Color{28, 34, 52, 255};
        break;
    case 2:   // 描边玻璃：淡淡一层底 + 主题色高亮
        baseNorm = Color{255, 255, 255, 46};   // 20 太淡：深色背景上按钮会"消失"
        baseHover = Color{accent.r, accent.g, accent.b, 66};
        textBase = gPalette.text;
        break;
    default:  // 深色玻璃：底色随主题色偏移，半透明让星空透出来
        baseNorm = gPalette.surfaceAlt;
        baseNorm.a = 168;
        baseHover = mix(gPalette.surfaceHot, accent, 0.45f);
        baseHover.a = 205;
        textBase = gPalette.text;
        break;
    }

    Color base = enabled ? lerpColor(baseNorm, baseHover, e) : gPalette.surfaceSunken;
    // 描边样式的边框单独给亮一点（palette().line 是白 alpha 40，深色底上等于没有）
    Color borderNorm = (gButtonStyle == 2) ? Color{255, 255, 255, 96} : gPalette.line;
    Color borderHover{static_cast<unsigned char>(accent.r * 0.65f + 255 * 0.35f),
                      static_cast<unsigned char>(accent.g * 0.65f + 255 * 0.35f),
                      static_cast<unsigned char>(accent.b * 0.65f + 255 * 0.35f), 155};
    Color border = enabled ? lerpColor(borderNorm, borderHover, e) : Color{255, 255, 255, 25};
    float r = std::min(gCornerRadius, rect.height * 0.24f);   // 圆角矩形，不做胶囊

    // 按下：在悬停色基础上再压暗一档，而不是瞬间跳到另一个颜色
    if (pressed)
    {
        base = (gButtonStyle == 1)
                   ? Color{static_cast<unsigned char>(base.r * 0.82f),
                           static_cast<unsigned char>(base.g * 0.82f),
                           static_cast<unsigned char>(base.b * 0.82f), base.a}
                   : Color{static_cast<unsigned char>(base.r * 0.62f),
                           static_cast<unsigned char>(base.g * 0.62f),
                           static_cast<unsigned char>(base.b * 0.62f), base.a};
    }

    // 整体透明度（用于抽屉打开时让菜单按钮平滑淡出，而不是"啪"地消失）
    base.a = static_cast<unsigned char>(base.a * alpha);
    border.a = static_cast<unsigned char>(border.a * alpha);

    // 质感：竖直渐变（上亮下暗）+ 顶部内高光 + 底部内阴影 + 柔和投影 + 悬停外发光。
    // 悬停时整块微微"抬起来"（放大 1.5px、投影变深），按下时"压下去"（缩小 1px）。
    // 悬停：整块轻轻抬起；按下：压扁（横向收 2%、纵向收 3%，面积略微变小 = 被按进去）
    renderer::Deform press;
    if (enabled)
    {
        press.sx = 1.0f + 0.010f*e - (pressed ? 0.020f : 0.0f);
        press.sy = 1.0f + 0.016f*e - (pressed ? 0.030f : 0.0f);
    }
    const Rectangle body = renderer::deformRect(rect, press, 0.5f, 0.5f);
    const float lift = (body.height - rect.height)*0.5f;   // 文字跟着"抬起/压下"的量
    const float bodyRadius = std::min(r*(press.sx + press.sy)*0.5f, body.height*0.5f);

    Color top = mix(base, Color{255, 255, 255, base.a}, 0.10f);
    Color bottom = mix(base, Color{0, 0, 0, base.a}, pressed ? 0.16f : 0.09f);
    // 玻璃按钮：顶部高光更亮一点（厚度感），底部压暗更明显
    const float rim = (gButtonStyle == 2) ? 0.08f : 0.17f;
    const float shade = (gButtonStyle == 1) ? 0.05f : 0.08f;
    const float shadow = enabled ? (0.30f + 0.16f * e - (pressed ? 0.16f : 0.0f)) * alpha : 0.0f;
    Color glow = enabled ? Color{accent.r, accent.g, accent.b,
                                 static_cast<unsigned char>(std::min(1.0f, e*0.40f) * alpha * 255.0f)}
                         : Color{0, 0, 0, 0};
    drawButtonSurface(body, bodyRadius, top, bottom, border, 1.5f, rim, shade,
                      shadow, glow, 7.0f + 4.0f*e);

    // 悬停时再叠一层主题色薄雾，让"选中感"更明确（按下的按钮不叠）
    if (e > 0.01f && enabled && !pressed)
    {
        DrawRectangleRounded(body, roundnessOf(bodyRadius, body), 16,
                             Color{accent.r, accent.g, accent.b,
                                   static_cast<unsigned char>(40 * e * alpha)});
    }

    if (enabled)
    {
        Vector2 m = MeasureTextEx(font, label.c_str(), fontSize, fontSize / 10.0f);
        unsigned char ta = static_cast<unsigned char>((215 + 40 * e) * alpha);
        Color textColor = textBase;
        textColor.a = ta;
        // 文字比底板再上移 1px：视觉重心偏上一点更好看（按钮抬起时一起动）
        DrawTextEx(font, label.c_str(),
                   {body.x + (body.width - m.x) * 0.5f,
                    body.y + (body.height - m.y) * 0.5f - 3.0f - lift*0.5f},
                   fontSize, fontSize / 10.0f, textColor);
    }
    return enabled && pressed;
}

float drawSlider(Rectangle rect, float value01, bool drag, Color accent)
{
    accent = resolveAccent(accent);
    value01 = value01 < 0 ? 0 : (value01 > 1 ? 1 : value01);
    float trackY = rect.y + rect.height * 0.5f - 4.0f;
    DrawRectangleRounded({rect.x, trackY, rect.width, 8.0f}, 4.0f, 12,
                         Color{255, 255, 255, 26});
    float fillW = rect.width * value01;
    if (fillW > 8.0f)
        DrawRectangleRounded({rect.x, trackY, fillW, 8.0f}, 4.0f, 12, accent);

    Vector2 knob(rect.x + rect.width * value01, rect.y + rect.height * 0.5f);
    float kr = drag ? 15.0f : 13.0f;
    DrawCircleV(knob, kr + 4.0f, Color{0, 0, 0, 60});
    DrawCircleV(knob, kr, drag ? Color{255, 255, 255, 255} : Color{225, 232, 255, 255});
    DrawCircleV(knob, kr * 0.45f, accent);

    if (drag)
    {
        float mx = static_cast<float>(GetMouseX());
        value01 = (mx - rect.x) / rect.width;
        value01 = value01 < 0 ? 0 : (value01 > 1 ? 1 : value01);
    }
    return value01;
}

bool drawToggle(Rectangle rect, bool on, bool clicked, Color accent)
{
    accent = resolveAccent(accent);
    Color offFill = gPalette.surfaceHot;
    offFill.a = 220;
    Color fill = on ? accent : offFill;
    Color border = Color{255, 255, 255, 70};
    float rr = roundnessOf(std::min(gCornerRadius, rect.height * 0.5f), rect);
    DrawRectangleRounded(rect, rr, 16, fill);
    drawRoundedBorder(rect, std::min(gCornerRadius, rect.height * 0.5f), 1.5f, border);
    float pad = 4.0f;
    float d = rect.height - pad * 2.0f;
    float kx = on ? rect.x + rect.width - d - pad : rect.x + pad;
    DrawCircleV({kx + d * 0.5f, rect.y + rect.height * 0.5f}, d * 0.5f,
                Color{255, 255, 255, 245});
    if (clicked) on = !on;
    return on;
}

void drawAccentLine(Rectangle rect, Color color, float width)
{
    DrawRectangleRounded({rect.x + rect.width * 0.08f, rect.y,
                          rect.width * 0.84f, width},
                         width * 0.5f, 4, color);
}

namespace
{
Color gradientStep(Color a, Color b, float t)
{
    return Color{
        static_cast<unsigned char>(a.r + (b.r - a.r) * t),
        static_cast<unsigned char>(a.g + (b.g - a.g) * t),
        static_cast<unsigned char>(a.b + (b.b - a.b) * t),
        static_cast<unsigned char>(a.a + (b.a - a.a) * t),
    };
}
}

void drawShine(Rectangle rect, float radiusPx, float progress, float strength)
{
    if (rect.width < 4.0f || rect.height < 4.0f || strength <= 0.01f) return;
    if (!ensureShineShader()) return;

    const float panel[2] = {rect.width, rect.height};
    BeginBlendMode(BLEND_ADDITIVE);   // 加色：只在卡片上"加一道光"
    BeginShaderMode(gShineShader);
    SetShaderValue(gShineShader, gShineProgress, &progress, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gShineShader, gShineStrength, &strength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gShineShader, gShinePanel, panel, SHADER_UNIFORM_VEC2);
    SetShaderValue(gShineShader, gShineRadius, &radiusPx, SHADER_UNIFORM_FLOAT);
    DrawTexturePro(GetShapesTexture(), {0.0f, 0.0f, 1.0f, 1.0f}, rect,
                   {0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
    EndBlendMode();
}

void drawGradientH(Rectangle rect, Color c1, Color c2, int strips)
{
    if (strips <= 0) strips = 64;
    float w = rect.width / static_cast<float>(strips);
    for (int i = 0; i < strips; ++i)
    {
        float t = (i + 0.5f) / static_cast<float>(strips);
        int x0 = static_cast<int>(rect.x + i * w);
        int x1 = static_cast<int>(rect.x + (i + 1) * w);
        if (x1 <= x0) x1 = x0 + 1;
        DrawRectangle(x0, static_cast<int>(rect.y), x1 - x0, static_cast<int>(rect.height),
                      gradientStep(c1, c2, t));
    }
}

void drawGradientV(Rectangle rect, Color c1, Color c2, int strips)
{
    if (strips <= 0) strips = 64;
    float h = rect.height / static_cast<float>(strips);
    for (int i = 0; i < strips; ++i)
    {
        float t = (i + 0.5f) / static_cast<float>(strips);
        int y0 = static_cast<int>(rect.y + i * h);
        int y1 = static_cast<int>(rect.y + (i + 1) * h);
        if (y1 <= y0) y1 = y0 + 1;
        DrawRectangle(static_cast<int>(rect.x), y0,
                      static_cast<int>(rect.width), y1 - y0,
                      gradientStep(c1, c2, t));
    }
}
}
