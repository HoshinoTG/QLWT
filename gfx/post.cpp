#include "gfx.h"

#include "gfx_internal.h"
#include "post.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// 后处理实现（详见 post.h 的流程说明）
// ---------------------------------------------------------------------------

namespace
{
// 后处理自带的顶点着色器：属性名与默认着色器一致，mvp 由 batch 自动下发
const char* kPostVert = R"(#version 330
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

// ① 亮部提取（顺便 4 点降采样到 1/4 分辨率）
const char* kBrightFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 uTexel;        // 1 / 场景尺寸
uniform float uThreshold;   // 亮度阈值
out vec4 finalColor;
void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb*0.25;
    c += texture(texture0, fragTexCoord + vec2(uTexel.x, 0.0)).rgb*0.25;
    c += texture(texture0, fragTexCoord + vec2(0.0, uTexel.y)).rgb*0.25;
    c += texture(texture0, fragTexCoord + uTexel).rgb*0.25;

    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // 软膝：阈值附近平滑过渡，避免"刚好过线"的亮色 UI 突然爆白
    float w = smoothstep(uThreshold, min(uThreshold + 0.18, 1.0), luma);
    finalColor = vec4(c*w, 1.0);
}
)";

// ②③ 可分离高斯模糊（线性采样的 5 抽头）
const char* kBlurFrag = R"(#version 330
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

// ⑤ 合成：转场遮罩 → 调色 → 暗角 → 胶片颗粒
const char* kCompositeFrag = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform float uProgress;     // 转场进度 0..1
uniform int uTransition;     // 0=无 1=黑场 2=溶解 3=径向 4=白闪
uniform vec2 uCenter;        // 转场中心（画布 uv，左上原点）
uniform vec4 uFade;          // 转场色（rgb + 强度）
uniform float uVignette;     // 暗角 0..1
uniform float uGrain;        // 胶片颗粒 0..1
uniform float uSaturation;   // 饱和度（1 = 不变）
uniform float uContrast;     // 对比度（1 = 不变）
uniform float uSeed;         // 每帧变化的随机种子
uniform float uAspect;       // 画布宽高比
uniform float uFlash;        // 白闪强度 0..1
uniform int uAaMode;         // 1 = FXAA（0/2 都不用后处理抗锯齿）
uniform vec2 uTexel;         // 1 / 场景尺寸
out vec4 finalColor;

// FXAA（简化版 3.11）：沿边缘方向做几次采样再混合，能压掉整幅画面的阶梯
vec3 fxaa(sampler2D tex, vec2 uv, vec2 texel)
{
    vec3 rgbM = texture(tex, uv).rgb;
    vec3 rgbNW = texture(tex, uv + vec2(-1.0, -1.0)*texel).rgb;
    vec3 rgbNE = texture(tex, uv + vec2( 1.0, -1.0)*texel).rgb;
    vec3 rgbSW = texture(tex, uv + vec2(-1.0,  1.0)*texel).rgb;
    vec3 rgbSE = texture(tex, uv + vec2( 1.0,  1.0)*texel).rgb;

    const vec3 lumaW = vec3(0.299, 0.587, 0.114);
    float lumaM  = dot(rgbM,  lumaW);
    float lumaNW = dot(rgbNW, lumaW);
    float lumaNE = dot(rgbNE, lumaW);
    float lumaSW = dot(rgbSW, lumaW);
    float lumaSE = dot(rgbSE, lumaW);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE)*0.25*0.03125, 1.0/128.0);
    float rcpDirMin = 1.0/(min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir*rcpDirMin, vec2(-8.0), vec2(8.0))*texel;

    vec3 rgbA = 0.5*(texture(tex, uv + dir*(1.0/3.0 - 0.5)).rgb +
                     texture(tex, uv + dir*(2.0/3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA*0.5 + 0.25*(texture(tex, uv + dir*-0.5).rgb +
                                 texture(tex, uv + dir*0.5).rgb);
    float lumaB = dot(rgbB, lumaW);
    return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

float hash21(vec2 p)
{
    p = fract(p*vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x*p.y);
}

// 值噪声：转场边缘用它才不会是一条直线
float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f*f*(3.0 - 2.0*f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main()
{
    vec3 col = (uAaMode == 1) ? fxaa(texture0, fragTexCoord, uTexel)
                              : texture(texture0, fragTexCoord).rgb;
    vec2 uv = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);   // 转成左上原点
    vec2 aspect = vec2(uAspect, 1.0);

    // ---- 转场遮罩：mask = 被"转场色"盖住的比例 ----
    float t = clamp(uProgress, 0.0, 1.0);
    if (uTransition != 0 && t > 0.0 && t < 1.0)
    {
        float inPhase = (t >= 0.5) ? 1.0 : 0.0;
        float p = (inPhase > 0.5) ? (t - 0.5)*2.0 : t*2.0;
        float appear = (inPhase > 0.5) ? p : (1.0 - p);     // 画面露出比例
        float mask = 0.0;

        if (uTransition == 1)
        {
            mask = 1.0 - appear;                            // 黑场淡入淡出
        }
        else if (uTransition == 2)
        {
            float n = valueNoise(uv*aspect*70.0 + 13.7);    // 噪点溶解
            mask = smoothstep(appear - 0.16, appear + 0.16, n);
        }
        else if (uTransition == 3)
        {
            // 径向擦除：以 uCenter 为圆心的圆环往外扫。
            // 退场阶段黑圈从中心涨开，入场阶段黑圈继续往外退 -> 整段是一次连续的外扩
            vec2 d = (uv - uCenter)*aspect;
            float r = length(d)/max(length(aspect*0.5)*1.15, 0.0001);
            mask = 1.0 - smoothstep(p - 0.14, p + 0.14, r);
        }
        else if (uTransition == 4)
        {
            mask = pow(1.0 - abs(t*2.0 - 1.0), 1.7);        // 白闪：中间最亮
        }

        col = mix(col, uFade.rgb, clamp(mask, 0.0, 1.0)*uFade.a);
    }

    // ---- 白闪（CG 揭示、战斗开始）----
    if (uFlash > 0.0)
    {
        col = mix(col, vec3(1.0), clamp(uFlash, 0.0, 1.0));
    }

    // ---- 调色 ----
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(luma), col, uSaturation);
    col = (col - 0.5)*uContrast + 0.5;

    // ---- 暗角 ----
    vec2 vd = (uv - 0.5)*aspect;
    float rr = length(vd)/max(length(aspect*0.5), 0.0001);
    col *= mix(1.0, smoothstep(1.25, 0.45, rr), uVignette);

    // ---- 胶片颗粒（放在最后，像胶片一样盖在画面上）----
    if (uGrain > 0.0)
    {
        float g = hash21(uv*vec2(1920.0, 1080.0) + vec2(uSeed, uSeed*1.7)) - 0.5;
        col += g*uGrain;
    }

    finalColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)";

struct Post
{
    RenderTexture2D bloomA{};
    RenderTexture2D bloomB{};
    Shader bright{};
    Shader blur{};
    Shader compositeShader{};

    int locBrightTexel = -1;
    int locBrightThreshold = -1;
    int locBlurDir = -1;
    int locProgress = -1;
    int locTransition = -1;
    int locCenter = -1;
    int locFade = -1;
    int locVignette = -1;
    int locGrain = -1;
    int locSaturation = -1;
    int locContrast = -1;
    int locSeed = -1;
    int locAspect = -1;
    int locFlash = -1;
    int locAaMode = -1;
    int locTexel = -1;

    int width = 0;
    int height = 0;
    bool ready = false;
};

Post g;

// 画一整张贴图（沿用引擎的翻转约定：源矩形高度取负）
void blitFull(Texture2D texture, float width, float height)
{
    DrawTexturePro(texture,
                   {0.0f, 0.0f, static_cast<float>(texture.width),
                    -static_cast<float>(texture.height)},
                   {0.0f, 0.0f, width, height}, {0.0f, 0.0f}, 0.0f, WHITE);
}

void releaseTargets()
{
    if (g.bloomA.id) UnloadRenderTexture(g.bloomA);
    if (g.bloomB.id) UnloadRenderTexture(g.bloomB);
    g.bloomA = RenderTexture2D{};
    g.bloomB = RenderTexture2D{};
}

void releaseShaders()
{
    if (g.bright.id) UnloadShader(g.bright);
    if (g.blur.id) UnloadShader(g.blur);
    if (g.compositeShader.id) UnloadShader(g.compositeShader);
    g.bright = Shader{};
    g.blur = Shader{};
    g.compositeShader = Shader{};
}
}   // namespace

namespace postfx
{
float defaultBloom(int quality)
{
    return quality >= QualityHigh ? 0.62f : (quality == QualityLow ? 0.40f : 0.0f);
}

void shutdown()
{
    releaseTargets();
    releaseShaders();
    g.ready = false;
    g.width = 0;
    g.height = 0;
}

void init(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (g.ready && g.width == width && g.height == height) return;

    shutdown();
    g.width = width;
    g.height = height;

    // 泛光在 1/4 分辨率上算：1920x1080 -> 480x270，软件渲染也很快
    const int bw = std::max(1, width/4);
    const int bh = std::max(1, height/4);
    g.bloomA = LoadRenderTexture(bw, bh);
    g.bloomB = LoadRenderTexture(bw, bh);

    g.bright = LoadShaderFromMemory(kPostVert, kBrightFrag);
    g.blur = LoadShaderFromMemory(kPostVert, kBlurFrag);
    g.compositeShader = LoadShaderFromMemory(kPostVert, kCompositeFrag);

    g.locBrightTexel = GetShaderLocation(g.bright, "uTexel");
    g.locBrightThreshold = GetShaderLocation(g.bright, "uThreshold");
    g.locBlurDir = GetShaderLocation(g.blur, "uDir");
    g.locProgress = GetShaderLocation(g.compositeShader, "uProgress");
    g.locTransition = GetShaderLocation(g.compositeShader, "uTransition");
    g.locCenter = GetShaderLocation(g.compositeShader, "uCenter");
    g.locFade = GetShaderLocation(g.compositeShader, "uFade");
    g.locVignette = GetShaderLocation(g.compositeShader, "uVignette");
    g.locGrain = GetShaderLocation(g.compositeShader, "uGrain");
    g.locSaturation = GetShaderLocation(g.compositeShader, "uSaturation");
    g.locContrast = GetShaderLocation(g.compositeShader, "uContrast");
    g.locSeed = GetShaderLocation(g.compositeShader, "uSeed");
    g.locAspect = GetShaderLocation(g.compositeShader, "uAspect");
    g.locFlash = GetShaderLocation(g.compositeShader, "uFlash");
    g.locAaMode = GetShaderLocation(g.compositeShader, "uAaMode");
    g.locTexel = GetShaderLocation(g.compositeShader, "uTexel");

    g.ready = g.bloomA.id != 0 && g.bloomB.id != 0 && g.bright.id != 0 &&
              g.blur.id != 0 && g.compositeShader.id != 0;
    if (!g.ready)
        std::fprintf(stderr, "[postfx] 后处理初始化失败（%dx%d），本帧起退回直接贴图\n",
                     width, height);
}

void composite(Texture2D scene, RenderTexture2D sceneTarget, Rectangle dest,
               const Frame& frame)
{
    const bool wantsPost = (frame.quality != QualityOff || frame.aa != AaOff) && g.ready &&
                           dest.width > 0.0f && dest.height > 0.0f;
    // 什么都没开：直接贴图（和替换前的画法逐像素一致）
    if (!wantsPost)
    {
        DrawTexturePro(scene,
                       {0.0f, 0.0f, static_cast<float>(scene.width),
                        -static_cast<float>(scene.height)},
                       dest, {0.0f, 0.0f}, 0.0f, WHITE);
        return;
    }

    const float bloomStrength = defaultBloom(frame.quality);
    const float bw = static_cast<float>(g.bloomA.texture.width);
    const float bh = static_cast<float>(g.bloomA.texture.height);

    // ①②③ 泛光链（只在新画面基本可见时才做，避免转场黑屏时白算）
    if (bloomStrength > 0.0f && sceneTarget.id != 0)
    {
        // 亮部提取 + 降采样
        BeginTextureMode(g.bloomA);
        ClearBackground(BLACK);
        BeginShaderMode(g.bright);
        {
            // 4 个抽样点按「1/4 分辨率的一个像素」铺开，等于顺便做了一次盒式降采样
            const float texel[2] = {1.0f/bw, 1.0f/bh};
            const float threshold = 0.80f;   // 只有窗口高光 / 星空 / 白字这类才泛光
            SetShaderValue(g.bright, g.locBrightTexel, texel, SHADER_UNIFORM_VEC2);
            SetShaderValue(g.bright, g.locBrightThreshold, &threshold, SHADER_UNIFORM_FLOAT);
            blitFull(scene, bw, bh);
        }
        EndShaderMode();
        EndTextureMode();

        // 纵向/横向交替模糊；高质量档多来一轮、步长更大，光晕更宽
        const int iterations = (frame.quality >= QualityHigh) ? 2 : 1;
        for (int i = 0; i < iterations; ++i)
        {
            const float radius = (i == 0) ? 1.0f : 2.2f;
            const float stepX = radius/bw;
            const float stepY = radius/bh;

            BeginTextureMode(g.bloomB);
            ClearBackground(BLACK);
            BeginShaderMode(g.blur);
            {
                const float dir[2] = {stepX, 0.0f};
                SetShaderValue(g.blur, g.locBlurDir, dir, SHADER_UNIFORM_VEC2);
                blitFull(g.bloomA.texture, bw, bh);
            }
            EndShaderMode();
            EndTextureMode();

            BeginTextureMode(g.bloomA);
            ClearBackground(BLACK);
            BeginShaderMode(g.blur);
            {
                const float dir[2] = {0.0f, stepY};
                SetShaderValue(g.blur, g.locBlurDir, dir, SHADER_UNIFORM_VEC2);
                blitFull(g.bloomB.texture, bw, bh);
            }
            EndShaderMode();
            EndTextureMode();
        }

        // ④ 泛光按加色叠回场景（这样合成阶段只需要采样一张贴图）
        // 泛光叠回"场景所在的那张目标"：MSAA 时绘制目标是多采样 FBO，
        // 但场景贴图挂在解析 FBO 上，要叠到解析 FBO 才有效
        RenderTexture2D addTarget = sceneTarget;
        if (sceneTarget.resolveFbo != 0)
        {
            addTarget.id = sceneTarget.resolveFbo;
            addTarget.resolveFbo = 0;
        }
        BeginTextureMode(addTarget);
        BeginBlendMode(BLEND_ADDITIVE);
        BeginShaderMode(Shader{});   // 显式切回默认着色器：加色阶段只靠顶点色控强度
        {
            // 默认着色器算的是 texel*colDiffuse*fragColor，顶点色的 alpha 就是强度
            const unsigned char s = static_cast<unsigned char>(std::min(255.0f, bloomStrength*255.0f));
            DrawTexturePro(g.bloomA.texture,
                           {0.0f, 0.0f, bw, -bh},
                           {0.0f, 0.0f, static_cast<float>(sceneTarget.texture.width),
                            static_cast<float>(sceneTarget.texture.height)},
                           {0.0f, 0.0f}, 0.0f, Color{255, 255, 255, s});
        }
        EndShaderMode();
        EndBlendMode();
        EndTextureMode();
    }

    // ⑤ 合成到屏幕：转场 + 调色 + 暗角 + 颗粒
    const float progress = frame.progress;
    const int transition = frame.transition;
    const float center[2] = {frame.centerX, frame.centerY};
    const float fade[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // 黑场/溶解/径向都用黑
    const float flash[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // 白闪用白
    const float vignette = (frame.quality >= QualityHigh) ? 0.34f : 0.22f;
    const float grain = (frame.quality >= QualityHigh) ? 0.028f : 0.016f;
    const float saturation = 1.06f;
    const float contrast = 1.03f;
    const float seed = static_cast<float>(frame.seed);

    BeginShaderMode(g.compositeShader);
    SetShaderValue(g.compositeShader, g.locProgress, &progress, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locTransition, &transition, SHADER_UNIFORM_INT);
    SetShaderValue(g.compositeShader, g.locCenter, center, SHADER_UNIFORM_VEC2);
    SetShaderValue(g.compositeShader, g.locFade,
                   (transition == TransitionFlash) ? flash : fade, SHADER_UNIFORM_VEC4);
    SetShaderValue(g.compositeShader, g.locVignette, &vignette, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locGrain, &grain, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locSaturation, &saturation, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locContrast, &contrast, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locSeed, &seed, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locAspect, &frame.aspect, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g.compositeShader, g.locFlash, &frame.flash, SHADER_UNIFORM_FLOAT);
    const float aaTexel[2] = {1.0f/static_cast<float>(scene.width),
                              1.0f/static_cast<float>(scene.height)};
    SetShaderValue(g.compositeShader, g.locAaMode, &frame.aa, SHADER_UNIFORM_INT);
    SetShaderValue(g.compositeShader, g.locTexel, aaTexel, SHADER_UNIFORM_VEC2);
    DrawTexturePro(scene,
                   {0.0f, 0.0f, static_cast<float>(scene.width),
                    -static_cast<float>(scene.height)},
                   dest, {0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
}
}   // namespace postfx
