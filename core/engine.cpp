#include "engine.h"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstdlib>

#include "canvas.h"
#include "gesture.h"
#include "../gfx/post.h"

#include "../game/scene.h"
#include "../renderer/renderer.h"

namespace
{
// 场景切换总时长（前半段退场 + 后半段入场各占一半）
constexpr float kTransitionSeconds = 0.36f;
}

bool Engine::init(int screenX, int screenY, const char* windowName,
                  const std::vector<std::string>& texts,
                  bool selftestMode, int frames)
{
    // VSync: 2x supersampling is done via an offscreen render target,
    // no post-process FXAA so text and thin borders stay clean.
    // 窗口允许自由缩放；场景始终按设计分辨率排版，最后整块画布等比缩放上屏
    // HIGHDPI：在 150%/200% 缩放的屏幕上按真实帧缓冲分辨率渲染（否则内容只有
    // 逻辑分辨率，被合成器放大后整屏发虚）。
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(screenX, screenY, windowName);
    if (!IsWindowReady()) return false;
    SetWindowMinSize(640, 360);
    // 禁用默认的 ESC 退出键，ESC 交给各场景处理（菜单/返回）
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    InitAudioDevice();

    settings.load("settings.cfg");
    if (settings.fullscreen) SetWindowState(FLAG_FULLSCREEN_MODE);
    applyTheme();

    fonts.init(texts);
    // 排版基准是固定的设计分辨率，screenX/screenY 只是初始窗口大小
    designW_ = kDesignWidth;
    designH_ = kDesignHeight;
    canvas::setDesign(designW_, designH_);
    canvas::update();
    rtW_ = 0;
    rtH_ = 0;
    recreateRenderTarget();
    selftest = selftestMode;
    selftestFrames = frames;
    printf("[engine] ready (design %dx%d, window %dx%d, render %dx%d)\n", designW_, designH_,
           GetScreenWidth(), GetScreenHeight(), GetRenderWidth(), GetRenderHeight());
    return true;
}

void Engine::run()
{
    while (!WindowShouldClose() && running_)
    {
        time.update();
        gesture::poll();                // 触摸板手势（双指拖动 / 捏合）先读进来
        canvas::update();               // 窗口大小可能被拖动改变，每帧重算缩放
        recreateRenderTarget();         // 画布在屏幕上变大时提高离屏分辨率
        renderer::beginFrame(time.delta());   // 控件动画统一用这一份 dt
        if (selftest && scene_) scene_->debugAuto(time.frame());
        scene_->update(time.delta());

        // 场景切换：前半段旧画面退场，正中间换场景，后半段新画面入场
        // （两段都在上屏的合成着色器里做，见 gfx/post.cpp）
        if (transitioning_)
        {
            transitionT_ += time.delta() / kTransitionSeconds;
            if (transitionT_ >= 0.5f && nextScene_)
            {
                scene_ = std::move(nextScene_);
                nextScene_.reset();
            }
            if (transitionT_ >= 1.0f)
            {
                transitionT_ = 1.0f;
                transitioning_ = false;
                transitionStyle_ = postfx::TransitionFade;   // 复位成默认，下次切换不带上一次的花样
            }
        }
        frameSeed_ = frameSeed_ * 1664525u + 1013904223u;
        if (flashTimer_ > 0.0f)
        {
            flashTimer_ -= time.delta();
            if (flashTimer_ <= 0.0f) flashTimer_ = 0.0f;
        }

        BeginDrawing();
        if (rt_.texture.id)
        {
            // 实时玻璃的折射源不在这里准备：面板绘制时 renderer 会自己把
            // "当前已经画好的画面"抓一份出来（见 renderer.cpp 的 captureGlassSource），
            // 所以场景不需要再额外渲一遍背景快照。

            // 2x supersampling: render the scene at double resolution into the
            // offscreen target, then scale it down to the window. All geometry
            // edges are naturally anti-aliased by the downsample.
            BeginTextureMode(rt_);
            ClearBackground(Color{0, 0, 0, 255});
            gfxPushMatrix();
            const float rs = renderScale();   // 离屏分辨率 / 设计分辨率
            gfxScalef(rs, rs, 1.0f);
            scene_->draw();
            // 特效 / 抗锯齿都关掉时保留老画法：直接在离屏里盖一层黑（转场由别处兜底）
            const bool usePost = settings.fxQuality > 0 || settings.aaMode > 0;
            if (transitioning_ && !usePost)
            {
                float a = renderer::easeInOut(transitionT_) * 255.0f;
                DrawRectangle(0, 0, designW_, designH_,
                              Color{0, 0, 0, static_cast<unsigned char>(a)});
            }
            gfxPopMatrix();
            EndTextureMode();

            // 上屏：先铺黑边，再把画布经过「后处理 + 转场」贴上去
            ClearBackground(Color{0, 0, 0, 255});
            const float sc = canvas::scale();
            postfx::Frame frame;
            frame.quality = usePost ? settings.fxQuality : postfx::QualityOff;
            frame.aa = usePost ? settings.aaMode : postfx::AaOff;
            frame.transition = (transitioning_ && usePost) ? transitionStyle_
                                                           : postfx::TransitionNone;
            frame.progress = transitioning_ ? renderer::easeInOut(transitionT_) : 0.0f;
            frame.centerX = transitionCenterX_;
            frame.centerY = transitionCenterY_;
            frame.aspect = static_cast<float>(designW_) / static_cast<float>(designH_);
            frame.seed = frameSeed_;
            // 白闪：开头最亮，然后快速衰减（平方衰减更像真实曝光）
            if (flashTimer_ > 0.0f && flashDuration_ > 0.0f)
            {
                const float k = flashTimer_ / flashDuration_;
                frame.flash = flashStrength_ * k * k;
            }
            postfx::composite(rt_.texture, rt_,
                              {canvas::offsetX(), canvas::offsetY(),
                               static_cast<float>(designW_) * sc,
                               static_cast<float>(designH_) * sc},
                              frame);
        }
        else
        {
            ClearBackground(Color{0, 0, 0, 255});
            gfxPushMatrix();
            gfxTranslatef(canvas::offsetX(), canvas::offsetY(), 0.0f);
            gfxScalef(canvas::scale(), canvas::scale(), 1.0f);
            scene_->draw();
            if (transitioning_)
            {
                float a = renderer::easeInOut(transitionT_) * 255.0f;
                DrawRectangle(0, 0, designW_, designH_,
                              Color{0, 0, 0, static_cast<unsigned char>(a)});
            }
            gfxPopMatrix();
        }
        // 自检截图：必须在交换缓冲之前把这一帧的像素读走
        if (!shotPending_.empty())
        {
            gfxFlushBatch();   // 把尚未提交的绘制批次刷出去
            // LoadImageFromScreen() 按「当前渲染目标」的真实像素尺寸取像
            // （这里是默认帧缓冲，也就是 HiDPI 下的 1920x1080），并已竖直翻转。
            const std::string name = "shot_" + shotPending_ + ".png";
            Image shot = LoadImageFromScreen();
            ExportImage(shot, name.c_str());
            UnloadImage(shot);
            printf("[engine] screenshot saved: %s (frame %d)\n", name.c_str(), time.frame());
            shotPending_.clear();
        }

        EndDrawing();
        if (scene_) scene_->afterDraw();

        if (selftest)
        {
            // 截标题那一帧的时机：默认第 20 帧，可用 QLWT_SHOT_FRAME 覆盖
            // （标题界面有入场淡入，想截到完全亮起来的画面就把它调大一点）
            static const int shotFrame = [] {
                const char* v = std::getenv("QLWT_SHOT_FRAME");
                return v ? std::atoi(v) : 20;
            }();
            if (time.frame() == shotFrame) selftestShot("title");
        }

        if (selftest && time.frame() > selftestFrames) break;
    }

    if (selftest)
    {
        printf(selftestPassed ? "SELFTEST OK\n" : "SELFTEST FAILED (timeout)\n");
        if (std::getenv("QLWT_KEEP_SHOTS") == nullptr)
            cleanupSelftestFiles();
    }

    CloseAudioDevice();
    postfx::shutdown();
    CloseWindow();
}

void Engine::applyTheme()
{
    renderer::setTheme(renderer::hexToColor(settings.uiAccent),
                       static_cast<float>(settings.uiCorner),
                       settings.uiButtonStyle);
}

void Engine::flash(float strength, float duration)
{
    flashStrength_ = std::min(1.0f, std::max(0.0f, strength));
    flashDuration_ = duration > 0.01f ? duration : 0.01f;
    flashTimer_ = flashDuration_;
}

void Engine::setTransition(int style, float centerX, float centerY)
{
    transitionStyle_ = style;
    transitionCenterX_ = centerX;
    transitionCenterY_ = centerY;
}

void Engine::switchScene(std::shared_ptr<Scene> scene)
{
    if (!scene_ || transitioning_)
    {
        nextScene_ = std::move(scene);
        if (!scene_)
        {
            // 第一个场景：直接上，但补一段"从黑场进来"的入场
            scene_ = std::move(nextScene_);
            nextScene_.reset();
            transitioning_ = true;
            transitionT_ = 0.5f;
        }
        return;
    }
    nextScene_ = std::move(scene);
    transitioning_ = true;
    transitionT_ = 0.0f;
}

void Engine::recreateRenderTarget()
{
    // 离屏分辨率跟着画布在屏幕上的实际大小走：
    //   * 小窗口时至少 2x（超采样，边缘更干净）
    //   * 大窗口 / 全屏时提到 3x、4x，保证上屏是缩小而不是放大（放大就发虚）
    // 用「帧缓冲像素 / 设计像素」来定离屏分辨率：高 DPI 屏幕上帧缓冲比逻辑窗口大，
    // 只按逻辑缩放算的话上屏仍会被放大。
    const float fbW = static_cast<float>(GetRenderWidth());
    const float fbH = static_cast<float>(GetRenderHeight());
    const float screenScale = std::min(fbW / static_cast<float>(designW_),
                                       fbH / static_cast<float>(designH_));
    // 离屏分辨率 = 画布在屏幕上的实际像素尺寸，上屏就是 1:1 拷贝。
    // 以前的「整数倍超采样再缩回屏幕」会让文字经历两次重采样（字形图集先放大、
    // 整屏再缩回），边缘发虚；1:1 既最清晰，也省掉了整屏重采样的开销。
    const int w = std::max(1, static_cast<int>(screenScale * designW_ + 0.5f));
    const int h = std::max(1, static_cast<int>(screenScale * designH_ + 0.5f));
    // 抗锯齿档位会改变离屏目标的类型（多采样 / 普通），变了就得重建
    const bool wantMsaa = settings.aaMode >= 2;
    if (w == rtW_ && h == rtH_ && wantMsaa == (rt_.samples > 1)) return;
    if (rt_.texture.id) UnloadRenderTexture(rt_);
    rt_ = wantMsaa ? LoadRenderTextureMSAA(w, h, 4) : LoadRenderTexture(w, h);
    SetTextureFilter(rt_.texture, TEXTURE_FILTER_BILINEAR);

    // 毛玻璃取样用的场景快照：1/2 设计分辨率。分辨率越低越省、模糊越柔，
    // 边缘用 CLAMP，避免面板靠近画面边缘时高斯采样绕回另一侧。

    rtW_ = w;
    rtH_ = h;

    // 实时玻璃的抓取尺寸按设计分辨率算（HiDPI 下 RT 比设计大，比例不能混）
    renderer::setDesignSize(designW_, designH_);
    // 后处理缓冲跟着离屏尺寸走（1/4 分辨率泛光链）
    postfx::init(w, h);
}

void Engine::selftestShot(const std::string& tag)
{
    if (!selftest) return;
    // 真正的取像放到 run() 里、EndDrawing()（交换缓冲）之前做：
    // 交换之后再读帧缓冲，大概率只能拿到黑屏。
    shotPending_ = tag;
}

void Engine::cleanupSelftestFiles()
{
    std::error_code ec;
    std::filesystem::remove("progress.dat", ec);
    std::filesystem::remove("settings.cfg", ec);
    std::filesystem::remove_all("saves", ec);
    for (const auto& entry : std::filesystem::directory_iterator("."))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind("shot_", 0) == 0 && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".png") == 0)
            std::filesystem::remove(entry.path(), ec);
    }
}
