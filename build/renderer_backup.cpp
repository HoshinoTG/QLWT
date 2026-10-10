#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "gfx/gfx.h"

namespace renderer
{
namespace
{
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
uniform vec2 uTexel;      // 1 / 快照尺寸
uniform float uStep;      // 采样间隔（快照纹素）
uniform int uSamples;     // 单边采样数
uniform vec2 uUVMin;      // 面板左上角在快照里的 uv
uniform vec2 uUVSize;     // 面板在快照里的 uv 尺寸
uniform vec2 uPanelSize;  // 面板尺寸（设计像素）
uniform float uRadius;    // 圆角半径（设计像素）
uniform vec4 uTint;       // 叠加色（a 为混入比例）
uniform vec2 uPxToUV;     // 1 设计像素 = 多少快照 uv
uniform float uRefract;   // 边缘折射强度（设计像素）
uniform float uSpecular;  // 边缘高光强度
uniform float uSheen;     // 顶部内侧薄光
uniform float uSaturate;  // 背景饱和度增益
out vec4 finalColor;

// 圆角矩形距离场：内部为负、边界为 0
float roundedBox(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 t = (fragTexCoord - uUVMin)/uUVSize;
    vec2 p = (t - 0.5)*uPanelSize;
    vec2 halfSize = uPanelSize*0.5;
    float r = min(uRadius, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    float d = roundedBox(p, halfSize, r);
    float aa = max(fwidth(d), 0.6);
    float a = 1.0 - smoothstep(-aa, aa, d);

    // 边界法线（圆角处也对）：边缘折射和高光都要用
    vec2 grad = normalize(max(q, 0.0)*sign(p) + vec2(1e-6));

    // ---- 液态玻璃：越靠边，取样点越往法线方向偏，背后画面像被玻璃边缘"掰弯" ----
    float edge = 1.0 - smoothstep(0.0, uRefract*2.5, -d);
    vec2 refr = grad*uRefract*edge*uPxToUV;

    // 二维高斯：先沿 x 再沿 y 逐步叠加（权重为可分离分布的乘积）
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
            vec2 off = vec2(float(i), float(j))*uTexel*uStep;
            acc += texture(texture0, fragTexCoord + off + refr).rgb*w;
            wsum += w;
        }
    }
    vec3 col = acc/wsum;

    // 玻璃后面那层颜色更"实"一点，看着才像玻璃而不是灰雾
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(luma), col, uSaturate);
    col = mix(col, uTint.rgb, uTint.a);

    // ---- 高光：左上打光，边缘一圈亮边（液态玻璃的"厚度感"来源）----
    vec2 lightDir = normalize(vec2(-0.5, -0.86));
    float rimMask = 1.0 - smoothstep(0.0, 4.5, -d);
    float spec = pow(max(dot(grad, lightDir), 0.0), 2.5);
    col += vec3(uSpecular*spec*rimMask);
    col += vec3(uSheen*(1.0 - smoothstep(0.0, uPanelSize.y*0.28, p.y + halfSize.y))*rimMask);

    finalColor = vec4(clamp(col, 0.0, 1.0), a*fragColor.a);
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
    float edgeIn = -d;                                // 离边界往内多少像素

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

    // 内描边
    float borderA = (1.0 - smoothstep(uBorderW - aa, uBorderW + aa, edgeIn))*uBorder.a;
    vec3 body = mix(fill, uBorder.rgb, borderA);

    // 自下而上合成：投影(黑) -> 发光 -> 本体，最后反预乘交给硬件做常规 alpha 混合
    float sA = shadow*uShadow*(1.0 - inside);
    float gA = glow*uGlow.a*(1.0 - inside);
    float bA = inside*mix(uTop.a, uBottom.a, t);

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

    // 四边外扩要同时罩住「投影的模糊+下移」和「发光半径」，否则两块都会被裁出方边
    const float soft = 7.0f;
    const float drop = 3.0f;
    const float margin = std::max(2.0f, std::max(glowSpread + 2.0f, soft*2.0f + drop + 3.0f));
    const float panel[2] = {rect.width, rect.height};
    const float top4[4] = {top.r/255.0f, top.g/255.0f, top.b/255.0f, top.a/255.0f};
    const float bottom4[4] = {bottom.r/255.0f, bottom.g/255.0f, bottom.b/255.0f, bottom.a/255.0f};
    const float border4[4] = {border.r/255.0f, border.g/255.0f, border.b/255.0f, border.a/255.0f};
    const float glow4[4] = {glow.r/255.0f, glow.g/255.0f, glow.b/255.0f, glow.a/255.0f};

    BeginShaderMode(gButtonShader);
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

    DrawTexturePro(GetShapesTexture(), {0.0f, 0.0f, 1.0f, 1.0f},
                   {rect.x - margin, rect.y - margin,
                    rect.width + 2.0f*margin, rect.height + 2.0f*margin},
                   {0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
}

Texture2D gBackdrop{};
float gBackdropScale = 0.5f;

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

void beginFrame(float dt)
{
    gFrameDelta = dt < 0.0f ? 0.0f : (dt > 0.05f ? 0.05f : dt);
}

float frameDelta() { return gFrameDelta; }

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

void setBlurBackdrop(Texture2D backdrop, float texelsPerPixel)
{
    const float scale = texelsPerPixel > 0.0f ? texelsPerPixel : 1.0f;
    if (backdrop.id == 0)
    {
        gBackdrop = Texture2D{};
        gBackdropScale = scale;
        return;
    }

    // 立刻做一次预模糊：模糊后的贴图是快照的 1/2 分辨率（也就是画布的 1/4）
    preBlurBackdrop(backdrop);
    if (gPreBlurReady)
    {
        gBackdrop = gPreBlurB.texture;
        gBackdropScale = scale*0.5f;
    }
    else
    {
        gBackdrop = backdrop;
        gBackdropScale = scale;
    }
}

bool blurBackdropReady() { return gBackdrop.id != 0; }

void drawBlurPanel(Rectangle rect, Color tint, float radiusPx,
                   int blurSamples, float blurStep)
{
    if (rect.width < 2.0f || rect.height < 2.0f) return;
    if (!ensureBlurShader()) return;

    // 面板在设计空间里的位置 -> 快照纹理坐标
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

    // 背景已经预模糊过：面板**单次采样**即可（双线性插值本身就是一次柔化），
    // 这样玻璃的开销和面板大小无关 —— 设置抽屉 0.8M 像素也只有一次采样。
    // blurSamples/blurStep 仍然保留在签名里，方便将来需要额外补糊时再打开。
    (void)blurSamples;
    (void)blurStep;
    const int samples = 0;
    const float step = 1.0f;
    // blurStep 以「设计像素」为单位，换算成快照纹素；这样离屏/快照分辨率怎么变，
    // 模糊的视觉半径都不变。
    const float stepTexels = step * gBackdropScale;
    const float texel[2] = {1.0f / texW, 1.0f / texH};
    // DrawTexturePro 内部实际使用的起点是 src.y + srcH = texH - srcY，
    // uv 在竖直方向递减（尺寸取负），着色器里的局部坐标才是 0..1。
    const float uvMin[2] = {srcX / texW, (texH - srcY) / texH};
    const float uvSize[2] = {srcW / texW, -srcH / texH};
    const float panel[2] = {rect.width, rect.height};
    const float tintv[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f,
                            tint.a / 255.0f};
    // 液态玻璃参数：边缘折射 3.5px、左上高光、背后颜色提一点饱和、顶部一层薄光
    const float pxToUV[2] = {gBackdropScale / texW, gBackdropScale / texH};
    const float refract = 3.5f;
    const float specular = 0.17f;
    const float sheen = 0.045f;
    const float saturate = 1.18f;

    BeginShaderMode(gBlurShader);
    SetShaderValue(gBlurShader, gLocTexel, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocStep, &stepTexels, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSamples, &samples, SHADER_UNIFORM_INT);
    SetShaderValue(gBlurShader, gLocUVMin, uvMin, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocUVSize, uvSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocPanel, panel, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocRadius, &radiusPx, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocTint, tintv, SHADER_UNIFORM_VEC4);
    SetShaderValue(gBlurShader, gLocPxToUV, pxToUV, SHADER_UNIFORM_VEC2);
    SetShaderValue(gBlurShader, gLocRefract, &refract, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSpecular, &specular, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSheen, &sheen, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gBlurShader, gLocSaturate, &saturate, SHADER_UNIFORM_FLOAT);
    DrawTexturePro(gBackdrop, src, rect, {0, 0}, 0.0f, WHITE);
    EndShaderMode();
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
    case 1:   // 浅色：浅底深字
        baseNorm = mix(gPalette.surfaceAlt, Color{255, 255, 255, 255}, 0.88f);
        baseHover = mix(gPalette.surfaceHot, Color{255, 255, 255, 255}, 0.93f);
        baseNorm.a = 235;
        baseHover.a = 248;
        textBase = Color{28, 34, 52, 255};
        break;
    case 2:   // 描边：近乎透明底 + 主题色高亮
        baseNorm = Color{255, 255, 255, 14};
        baseHover = Color{accent.r, accent.g, accent.b, 52};
        textBase = gPalette.text;
        break;
    default:  // 深色：底色随主题色偏移
        baseNorm = gPalette.surfaceAlt;
        baseNorm.a = 230;
        baseHover = mix(gPalette.surfaceHot, accent, 0.45f);
        baseHover.a = 242;
        textBase = gPalette.text;
        break;
    }

    Color base = enabled ? lerpColor(baseNorm, baseHover, e) : gPalette.surfaceSunken;
    Color borderNorm = gPalette.line;
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
    const float lift = enabled ? (pressed ? -1.0f : 1.6f * e) : 0.0f;
    Rectangle body{rect.x - lift, rect.y - lift, rect.width + lift*2.0f, rect.height + lift*2.0f};
    const float bodyRadius = std::min(r + lift, body.height*0.5f);

    Color top = mix(base, Color{255, 255, 255, base.a}, 0.10f);
    Color bottom = mix(base, Color{0, 0, 0, base.a}, pressed ? 0.16f : 0.09f);
    const float rim = (gButtonStyle == 2) ? 0.05f : 0.11f;
    const float shade = (gButtonStyle == 1) ? 0.05f : 0.07f;
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
