#include "gallery_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gfx/gfx.h"

#include "../core/canvas.h"
#include "../core/engine.h"
#include "../core/lang.h"
#include "../core/save.h"
#include "../renderer/renderer.h"
#include "title_scene.h"

namespace
{
constexpr float kCardW = 190.0f;
constexpr float kCardH = 258.0f;
constexpr float kGapX = 18.0f;
constexpr float kGapY = 20.0f;
constexpr float kTop = 154.0f;
constexpr int kCols = 6;

const renderer::Palette& pal() { return renderer::palette(); }
Color withAlpha(Color c, unsigned char a) { c.a = a; return c; }
}

// 图集名单：出场顺序大致按主线推进。颜色 / 简介与剧本、素材清单保持一致。
const std::vector<GalleryScene::Entry>& GalleryScene::roster()
{
    static const std::vector<GalleryScene::Entry> t = {
        {"吴鸿韬", "wu_hongtao", "天文社代理社长 · 高二", "寡言、固执，嘴笨心细",
         Color{0x6F, 0xA8, 0xFF, 255}},
        {"黎璘", "li_lin", "图书馆委员 · 青梅竹马", "温柔、可靠，一个人记了两年云",
         Color{0xFF, 0x8F, 0xB0, 255}},
        {"张誉腾", "zhang_yuteng", "学生会会长", "公私分明、外冷内热",
         Color{0xE0, 0xB3, 0x4C, 255}},
        {"梁知奕", "liang_zhiyi", "转校生 · 物理竞赛部", "毒舌、傲娇，用数据当盔甲",
         Color{0x9B, 0x7B, 0xE0, 255}},
        {"尹博涛", "yin_botao", "田径队王牌", "热血、直率，在找新的赛道",
         Color{0xF0, 0x72, 0x4A, 255}},
        {"李君浩", "li_junhao", "高一学弟 · 钟表店之子", "怯懦、机械宅，手很巧",
         Color{0x4C, 0xC0, 0xA8, 255}},
        {"李俊辰", "li_junchen", "高三 · 问题学生", "冷淡、毒舌，画得一手好海报",
         Color{0x5A, 0x6B, 0x8C, 255}},
        {"邵清和", "shao_qinghe", "物理老师 · 天文社顾问", "倦怠、回避，十年前的那场雨",
         Color{0x7F, 0xB3, 0xD5, 255}},
        {"魏思远", "wei_siyuan", "前任社长 · 天文系在读", "洒脱、念旧，带回原始图纸",
         Color{0xD9, 0x8C, 0x5F, 255}},
        {"顾星禾", "gu_xinghe", "天文社社长 · 摄影", "拍会动的光，从记录到负责",
         Color{0xDA, 0x70, 0xD6, 255}},
        {"沈砚", "shen_yan", "高一 · 志愿讲解员", "背得下整本星表，结巴但认真",
         Color{0x9D, 0xBF, 0x5C, 255}},
    };
    return t;
}

GalleryScene::GalleryScene(Engine* e, std::string scriptPath, std::shared_ptr<Script> script,
                           std::string parseError)
    : Scene(e), scriptPath_(std::move(scriptPath)), parseError_(std::move(parseError)),
      script_(std::move(script))
{
    const auto& table = roster();
    seen_.resize(table.size(), 0);
    body_.assign(table.size(), Texture2D{});
    for (size_t i = 0; i < table.size(); ++i)
    {
        seen_[i] = galleryIsSeen(table[i].name) ? 1 : 0;
        if (seen_[i]) ++seenCount_;
    }
}

Rectangle GalleryScene::cardRect(int idx) const
{
    const float total = kCols * kCardW + (kCols - 1) * kGapX;
    const float x0 = (static_cast<float>(canvas::width()) - total) * 0.5f;
    const int col = idx % kCols;
    const int row = idx / kCols;
    return Rectangle{x0 + col * (kCardW + kGapX), kTop + row * (kCardH + kGapY),
                     kCardW, kCardH};
}

void GalleryScene::openDetail(int idx)
{
    const auto& table = roster();
    if (idx < 0 || idx >= static_cast<int>(table.size())) return;
    if (!seen_[idx]) return;                 // 没登场的不给看
    if (detail_ != idx) { detailT_ = 0.0f; detailSpring_.reset(0.0f); }
    detail_ = idx;
}

void GalleryScene::backToTitle()
{
    engine()->switchScene(
        std::make_shared<TitleScene>(engine(), scriptPath_, script_, parseError_));
}

void GalleryScene::update(float dt)
{
    const auto& table = roster();
    const int n = static_cast<int>(table.size());
    const bool pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const Vector2 mouse = GetMousePosition();

    // ---- 详情页 ----
    if (detail_ >= 0)
    {
        detailSpring_.update(detail_ >= 0 ? 1.0f : 0.0f, dt);
        detailT_ = detailSpring_.value;
        const bool close = IsKeyPressed(KEY_ESCAPE) ||
                           (detailT_ > 0.35f && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT));
        if (close) { detail_ = -1; return; }
        return;
    }

    // ---- 键盘选择 ----
    if (IsKeyPressed(KEY_RIGHT)) sel_ = (sel_ + 1) % n;
    if (IsKeyPressed(KEY_LEFT)) sel_ = (sel_ + n - 1) % n;
    if (IsKeyPressed(KEY_DOWN)) sel_ = (sel_ + kCols) % n;
    if (IsKeyPressed(KEY_UP)) sel_ = (sel_ + n - kCols) % n;
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) openDetail(sel_);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE))
    {
        backToTitle();
        return;
    }

    // ---- 鼠标悬停 / 点击 ----
    hover_ = -1;
    for (int i = 0; i < n; ++i)
    {
        const Rectangle r = cardRect(i);
        if (CheckCollisionPointRec(mouse, r)) hover_ = i;
        const float target = (hover_ == i || sel_ == i) ? 1.0f : 0.0f;
        hoverAnim_[i] = renderer::approach(hoverAnim_[i], target, 14.0f, dt);
    }
    if (pressed && hover_ >= 0)
    {
        sel_ = hover_;
        openDetail(hover_);
    }
}

void GalleryScene::draw()
{
    const auto& table = roster();
    const int n = static_cast<int>(table.size());
    const float w = static_cast<float>(canvas::width());
    const float h = static_cast<float>(canvas::height());
    const FontManager& fonts = engine()->fonts;

    // 底：深色渐变，和战斗 / 设置抽屉一个调子
    DrawRectangleGradientV(0, 0, static_cast<int>(w), static_cast<int>(h),
                           Color{10, 12, 22, 255}, Color{18, 22, 38, 255});

    // ---- 标题 ----
    const char* title = lang::tr("gallery.title");
    const Vector2 tm = fonts.measure(title, 40.0f);
    fonts.draw(title, (w - tm.x) * 0.5f, 34.0f, 40.0f, withAlpha(pal().text, 240), 3.0f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), lang::tr("gallery.count"), seenCount_, n);
    const Vector2 cm = fonts.measure(buf, 20.0f);
    fonts.draw(buf, (w - cm.x) * 0.5f, 88.0f, 20.0f, withAlpha(pal().textDim, 200), 2.0f);

    const char* hint = lang::tr("gallery.hint");
    fonts.draw(hint, 30.0f, h - 40.0f, 18.0f, withAlpha(pal().textMuted, 190), 1.8f);

    // ---- 卡片 ----
    for (int i = 0; i < n; ++i)
    {
        const Entry& e = table[i];
        const float lift = -8.0f * hoverAnim_[i];
        Rectangle r = cardRect(i);
        r.y += lift;
        const bool unlocked = seen_[i] != 0;

        DrawRectangleRounded(r, 14.0f, 12,
                             unlocked ? Color{26, 32, 54, 235} : Color{18, 20, 32, 235});
        if (hoverAnim_[i] > 0.01f)
            renderer::drawRoundedBorder(
                r, 14.0f, 2.5f,
                withAlpha(unlocked ? e.color : pal().line,
                          static_cast<unsigned char>(230 * hoverAnim_[i])));

        // 立绘：等比缩到卡片里，底部对齐（给名字留一条）
        if (unlocked)
        {
            if (body_[i].id == 0)
            {
                const std::string path = std::string("assets/char/") + e.id + "_body.png";
                if (FileExists(path.c_str()))
                {
                    body_[i] = LoadTexture(path.c_str());
                    if (body_[i].id)
                    {
                        GenTextureMipmaps(&body_[i]);
                        SetTextureFilter(body_[i], TEXTURE_FILTER_TRILINEAR);
                    }
                }
            }
            const Texture2D tex = body_[i];
            const float area = r.height - 46.0f;
            if (tex.id)
            {
                const float dh = area;
                const float dw = static_cast<float>(tex.width) *
                                 (dh / static_cast<float>(tex.height));
                const Rectangle dst{r.x + (r.width - dw) * 0.5f, r.y + 6.0f, dw, dh};
                BeginScissorMode(static_cast<int>(r.x * engine()->renderScale()),
                                 static_cast<int>(r.y * engine()->renderScale()),
                                 static_cast<int>(r.width * engine()->renderScale()),
                                 static_cast<int>((area + 6.0f) * engine()->renderScale()));
                DrawTexturePro(tex,
                               {0, 0, static_cast<float>(tex.width),
                                static_cast<float>(tex.height)},
                               dst, {0, 0}, 0.0f, WHITE);
                EndScissorMode();
            }
            else
            {
                const Vector2 nm = fonts.measure(e.name, 26.0f);
                fonts.draw(e.name, r.x + (r.width - nm.x) * 0.5f, r.y + area * 0.45f,
                           26.0f, withAlpha(e.color, 220), 2.4f);
            }
            const Vector2 nm = fonts.measure(e.name, 22.0f);
            fonts.draw(e.name, r.x + (r.width - nm.x) * 0.5f, r.y + r.height - 38.0f,
                       22.0f, withAlpha(e.color, 240), 2.1f);
        }
        else
        {
            // 未登场：剪影 + 问号
            const char* q = "？";
            const Vector2 qm = fonts.measure(q, 64.0f);
            fonts.draw(q, r.x + (r.width - qm.x) * 0.5f, r.y + 62.0f, 64.0f,
                       Color{80, 86, 108, 255}, 4.0f);
            const char* lock = lang::tr("gallery.locked");
            const Vector2 lm = fonts.measure(lock, 19.0f);
            fonts.draw(lock, r.x + (r.width - lm.x) * 0.5f, r.y + r.height - 38.0f,
                       19.0f, withAlpha(pal().textMuted, 170), 1.9f);
        }
    }

    // ---- 详情页 ----
    if (detail_ >= 0)
    {
        const Entry& e = table[detail_];
        const float t = std::clamp(detailT_, 0.0f, 1.0f);
        const float a = t * t * (3.0f - 2.0f * t);
        // 弹入：0.94 -> 1.0，弹簧的过冲让它略微"胀"过 1 再收住
        const float pop = 0.94f + 0.06f*detailT_;
        DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h),
                      Color{0, 0, 0, static_cast<unsigned char>(170 * a)});

        Rectangle panel{w * 0.5f - 430.0f, h * 0.5f - 250.0f + (1.0f - a) * 24.0f,
                        860.0f, 500.0f};
        panel = renderer::deformRect(panel, {pop, pop}, 0.5f, 0.5f);
        renderer::drawPanel(panel, withAlpha(Color{22, 26, 42, 255},
                                             static_cast<unsigned char>(245 * a)),
                            renderer::cornerRadius(), withAlpha(e.color, 190), 2.5f);

        // 立绘（贴左）
        const Texture2D tex = body_[detail_];
        if (tex.id)
        {
            const float dh = panel.height - 60.0f;
            const float dw = static_cast<float>(tex.width) *
                             (dh / static_cast<float>(tex.height));
            const float dx = panel.x + 70.0f + (330.0f - dw) * 0.5f;
            DrawTexturePro(tex,
                           {0, 0, static_cast<float>(tex.width),
                            static_cast<float>(tex.height)},
                           {dx, panel.y + 30.0f, dw, dh}, {0, 0}, 0.0f,
                           Color{255, 255, 255, static_cast<unsigned char>(255 * a)});
        }

        const float tx = panel.x + 430.0f;
        fonts.draw(e.name, tx, panel.y + 70.0f, 44.0f,
                   withAlpha(e.color, static_cast<unsigned char>(255 * a)), 3.2f);
        fonts.draw(e.role, tx, panel.y + 136.0f, 22.0f,
                   withAlpha(pal().text, static_cast<unsigned char>(225 * a)), 2.1f);
        fonts.draw(e.tagline, tx, panel.y + 190.0f, 24.0f,
                   withAlpha(pal().textDim, static_cast<unsigned char>(220 * a)), 2.2f);
        fonts.draw(lang::tr("gallery.detail_hint"), tx, panel.y + panel.height - 60.0f, 18.0f,
                   withAlpha(pal().textMuted, static_cast<unsigned char>(200 * a)), 1.8f);
    }

    // 入场淡入现在由引擎的转场统一负责
}

void GalleryScene::debugAuto(int frame)
{
    (void)frame;
    const auto& table = roster();
    ++dbgFrame_;
    // QLWT_GALLERY_DEMO=1：自检时先补齐几个"已登场"，让截图能看到真实效果
    // （progress.dat 在自检结束会被清掉）。正常用 --gallery 逛一圈不会往进度里塞东西。
    if (std::getenv("QLWT_GALLERY_DEMO") && dbgFrame_ == 2)
    {
        const int seedIdx[3] = {0, 1, 2};
        for (int i : seedIdx)
            galleryMarkSeen(table[i].name);
        seenCount_ = 0;
        for (size_t i = 0; i < table.size(); ++i)
        {
            seen_[i] = galleryIsSeen(table[i].name) ? 1 : 0;
            if (seen_[i]) ++seenCount_;
        }
    }
    else if (dbgFrame_ == 20) engine()->selftestShot("gallery");
    else if (dbgFrame_ == 26) openDetail(0);
    else if (dbgFrame_ == 40) engine()->selftestShot("gallery_detail");
    else if (dbgFrame_ == 46) engine()->selftestPassed = true;
}
