#include "gfx.h"

#include "gfx_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// 字体（stb_truetype）
//
// 对应 raylib 的 rtext：
//   * LoadFontEx：把「实际用到的码点」按 fontSize 光栅化成一张灰度图集，
//     并记录每个字形的 offsetX/offsetY/advanceX 与在图集里的矩形
//   * MeasureTextEx / DrawTextEx：度量与绘制算法和 raylib 逐行对齐，
//     否则文字排版（换行宽度、居中位置）会和以前不一样
//
// 字形度量的算法细节（和 raylib 一致，改动前请先确认 font.cpp 的探测逻辑）：
//   scaleFactor = stbtt_ScaleForPixelHeight(fontSize)
//   advanceX    = (int)(stbtt 的 advanceWidth * scaleFactor)   —— 直接截断
//   offsetY    += (int)(ascent * scaleFactor)
//   图集 padding = 4，图集尺寸按总面积推 2 的幂
// ---------------------------------------------------------------------------

namespace
{
constexpr int kGlyphPadding = 4;   // raylib 的 FONT_TTF_DEFAULT_CHARS_PADDING
constexpr int kDefaultFontSize = 32;
int gTextLineSpacing = 0;

// UTF-8：取下一个码点，返回码点值，*size 返回占用字节数
int codepointNext(const char* text, int* size)
{
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text);
    int length = 0;
    int value = 0;

    if (bytes[0] < 0x80) { value = bytes[0]; length = 1; }
    else if ((bytes[0] & 0xE0) == 0xC0) { value = bytes[0] & 0x1F; length = 2; }
    else if ((bytes[0] & 0xF0) == 0xE0) { value = bytes[0] & 0x0F; length = 3; }
    else if ((bytes[0] & 0xF8) == 0xF0) { value = bytes[0] & 0x07; length = 4; }
    else { value = bytes[0]; length = 1; }

    for (int i = 1; i < length; ++i) value = (value << 6) | (bytes[i] & 0x3F);
    if (size != nullptr) *size = length;
    return value;
}

// 字形图集：把每个字形的灰度位图按行依次排进一张图上（raylib 的 packMethod 0）
unsigned char* buildAtlas(const std::vector<unsigned char*>& bitmaps,
                          const std::vector<int>& widths, const std::vector<int>& heights,
                          int fontSize, int* atlasWidth, int* atlasHeight,
                          Rectangle* recs)
{
    const int padding = kGlyphPadding;
    const int glyphCount = static_cast<int>(bitmaps.size());

    int totalWidth = 0;
    for (int i = 0; i < glyphCount; ++i) totalWidth += widths[i] + 2 * padding;

    const int paddedFontSize = fontSize + 2 * padding;
    const float totalArea = static_cast<float>(totalWidth) * static_cast<float>(paddedFontSize) * 1.2f;
    const float imageMinSize = std::sqrt(totalArea);
    const int imageSize = static_cast<int>(std::pow(2.0f, std::ceil(std::log(imageMinSize) / std::log(2.0f))));

    int width = imageSize;
    int height = (totalArea < (static_cast<float>(imageSize * imageSize) / 2.0f)) ? imageSize / 2 : imageSize;
    if (width <= 0 || height <= 0) return nullptr;

    unsigned char* atlas = static_cast<unsigned char*>(std::calloc(static_cast<size_t>(width) * height, 1));
    if (atlas == nullptr) return nullptr;

    int offsetX = padding;
    int offsetY = padding;
    for (int i = 0; i < glyphCount; ++i)
    {
        if (offsetX >= (width - widths[i] - 2 * padding))
        {
            offsetX = padding;
            offsetY += paddedFontSize;
            if (offsetY > (height - fontSize - padding))
            {
                std::fprintf(stderr, "[gfx] 字体图集放不下第 %d 个字形\n", i);
                for (int j = i; j < glyphCount; ++j) recs[j] = Rectangle{};
                break;
            }
        }

        if (bitmaps[i] != nullptr)
        {
            for (int y = 0; y < heights[i]; ++y)
            {
                std::memcpy(atlas + static_cast<size_t>(offsetY + y) * width + offsetX,
                            bitmaps[i] + static_cast<size_t>(y) * widths[i],
                            static_cast<size_t>(widths[i]));
            }
        }

        recs[i] = Rectangle{static_cast<float>(offsetX), static_cast<float>(offsetY),
                            static_cast<float>(widths[i]), static_cast<float>(heights[i])};
        offsetX += widths[i] + 2 * padding;
    }

    *atlasWidth = width;
    *atlasHeight = height;
    return atlas;
}
}   // namespace

Font LoadFontEx(const char* fileName, int fontSize, int* codepoints, int codepointCount)
{
    Font font{};
    if (fileName == nullptr || fontSize <= 0) return font;

    // 读字体文件
    std::vector<unsigned char> fileData;
    {
        FILE* fp = std::fopen(fileName, "rb");
        if (fp == nullptr)
        {
            std::fprintf(stderr, "[gfx] 字体文件打不开：%s\n", fileName);
            return font;
        }
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (size <= 0)
        {
            std::fclose(fp);
            return font;
        }
        fileData.resize(static_cast<size_t>(size));
        const size_t read = std::fread(fileData.data(), 1, fileData.size(), fp);
        std::fclose(fp);
        if (read != fileData.size()) return font;
    }

    stbtt_fontinfo fontInfo{};
    if (!stbtt_InitFont(&fontInfo, fileData.data(), 0))
    {
        std::fprintf(stderr, "[gfx] 字体解析失败：%s\n", fileName);
        return font;
    }

    const int count = codepointCount > 0 ? codepointCount : 95;
    const float scaleFactor = stbtt_ScaleForPixelHeight(&fontInfo, static_cast<float>(fontSize));

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
    const int scaledAscent = static_cast<int>(static_cast<float>(ascent) * scaleFactor);

    std::vector<int> localCodepoints;
    if (codepoints == nullptr)
    {
        localCodepoints.resize(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) localCodepoints[i] = i + 32;
        codepoints = localCodepoints.data();
    }

    GlyphInfo* glyphs = static_cast<GlyphInfo*>(std::calloc(static_cast<size_t>(count), sizeof(GlyphInfo)));
    Rectangle* recs = static_cast<Rectangle*>(std::malloc(sizeof(Rectangle) * static_cast<size_t>(count)));
    if (glyphs == nullptr || recs == nullptr)
    {
        std::free(glyphs);
        std::free(recs);
        return font;
    }

    std::vector<unsigned char*> bitmaps(static_cast<size_t>(count), nullptr);
    std::vector<int> widths(static_cast<size_t>(count), 0);
    std::vector<int> heights(static_cast<size_t>(count), 0);

    int loaded = 0;
    for (int i = 0; i < count; ++i)
    {
        const int codepoint = codepoints[i];
        glyphs[i].value = codepoint;

        const int index = stbtt_FindGlyphIndex(&fontInfo, codepoint);
        if (index > 0)
        {
            int chw = 0, chh = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmap(
                &fontInfo, scaleFactor, scaleFactor, codepoint, &chw, &chh,
                &glyphs[i].offsetX, &glyphs[i].offsetY);
            if (bitmap != nullptr)
            {
                int advance = 0, bearing = 0;
                stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &advance, &bearing);
                glyphs[i].advanceX = static_cast<int>(static_cast<float>(advance) * scaleFactor);
                glyphs[i].offsetY += scaledAscent;

                bitmaps[i] = bitmap;
                widths[i] = chw;
                heights[i] = chh;
                ++loaded;
            }
        }

        // 空格：给一个"空的方块"，宽度用步进宽度（raylib 同样处理）
        if (codepoint == 32)
        {
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&fontInfo, 32, &advance, &bearing);
            glyphs[i].advanceX = static_cast<int>(static_cast<float>(advance) * scaleFactor);
            // 空格用一个"空白方块"占位（和图集里的 padding 一样全是 0）。
            // stbtt 为空格返回的位图是 0x0 的，尺寸被这里覆盖后不能再拿它去拷像素。
            if (bitmaps[i] != nullptr)
            {
                std::free(bitmaps[i]);
                bitmaps[i] = nullptr;
            }
            widths[i] = glyphs[i].advanceX > 0 ? glyphs[i].advanceX : 0;
            heights[i] = fontSize;
        }

        // 位图已经拷进图集，字形结构里不再保留像素（省内存）
        glyphs[i].image = Image{};
        glyphs[i].image.width = widths[i];
        glyphs[i].image.height = heights[i];
        glyphs[i].image.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
    }

    int atlasWidth = 0, atlasHeight = 0;
    unsigned char* atlas = buildAtlas(bitmaps, widths, heights, fontSize, &atlasWidth, &atlasHeight, recs);
    for (int i = 0; i < count; ++i)
    {
        if (bitmaps[i] != nullptr) std::free(bitmaps[i]);
    }
    if (atlas == nullptr)
    {
        std::free(glyphs);
        std::free(recs);
        return font;
    }

    // 图集转成 GRAY_ALPHA：R 恒为 255（白），A 是字形覆盖率。
    // 这一步很关键：默认着色器算的是 texel*colDiffuse*vertexColor，只有把
    // "覆盖率"放进 alpha，字形才是一块"白色 + 该处透明度"的贴图；
    // 若只给 GRAYSCALE，整块四边形（含字形四周的 padding）都会不透明，
    // 文字周围会出现一圈发灰/发黑的方块。（raylib 也是这么做的）
    {
        const size_t pixelCount = static_cast<size_t>(atlasWidth) * static_cast<size_t>(atlasHeight);
        unsigned char* grayAlpha = static_cast<unsigned char*>(std::malloc(pixelCount * 2));
        if (grayAlpha == nullptr)
        {
            std::free(atlas);
            std::free(glyphs);
            std::free(recs);
            return font;
        }
        for (size_t i = 0; i < pixelCount; ++i)
        {
            grayAlpha[i * 2] = 255;
            grayAlpha[i * 2 + 1] = atlas[i];
        }
        std::free(atlas);
        atlas = grayAlpha;
    }

    Image atlasImage{};
    atlasImage.data = atlas;
    atlasImage.width = atlasWidth;
    atlasImage.height = atlasHeight;
    atlasImage.mipmaps = 1;
    atlasImage.format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;

    // 调试用：QLWT_DUMP_FONT_ATLAS=<路径> 可以把字形图集导出成 PNG，
    // 方便和别的实现（例如以前的 raylib）逐像素对比字形光栅化结果
    if (const char* dump = std::getenv("QLWT_DUMP_FONT_ATLAS"); dump != nullptr && *dump)
    {
        ExportImage(atlasImage, dump);
        std::printf("[gfx] 字形图集已导出：%s\n", dump);
    }

    font.texture = LoadTextureFromImage(atlasImage);
    std::free(atlas);

    if (font.texture.id == 0)
    {
        std::free(glyphs);
        std::free(recs);
        return font;
    }

    font.baseSize = fontSize;
    font.glyphCount = count;
    font.glyphPadding = kGlyphPadding;
    font.recs = recs;
    font.glyphs = glyphs;

    std::printf("[gfx] 字体已加载：%s（%d 像素 | %d/%d 个字形 | 图集 %dx%d）\n",
                fileName, fontSize, loaded, count, atlasWidth, atlasHeight);
    return font;
}

void UnloadFont(Font font)
{
    if (font.texture.id != 0) UnloadTexture(font.texture);
    std::free(font.recs);
    std::free(font.glyphs);
}

Font GetFontDefault()
{
    // 兜底字体：只有在工程自带的 assets/fonts 都找不到时才会用到。
    // 这里尝试系统里常见的 TTF；都没有就返回空字体（文字不显示，但不崩）。
    static Font font = [] {
        const char* candidates[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
        };
        for (const char* path : candidates)
        {
            if (!FileExists(path)) continue;
            Font loaded = LoadFontEx(path, kDefaultFontSize, nullptr, 95);
            if (loaded.texture.id != 0) return loaded;
        }
        std::fprintf(stderr, "[gfx] 没有可用的兜底字体\n");
        return Font{};
    }();
    return font;
}

// 字形索引：找不到时回退到 '?'（与 raylib 一致）
int getGlyphIndex(Font font, int codepoint)
{
    int index = 0;
    int fallbackIndex = 0;
    for (int i = 0; i < font.glyphCount; ++i)
    {
        if (font.glyphs[i].value == 63) fallbackIndex = i;
        if (font.glyphs[i].value == codepoint)
        {
            index = i;
            break;
        }
    }
    if (index == 0 && font.glyphs[0].value != codepoint) index = fallbackIndex;
    return index;
}

Vector2 MeasureTextEx(Font font, const char* text, float fontSize, float spacing)
{
    Vector2 textSize{};
    if (text == nullptr || text[0] == '\0' || font.texture.id == 0 || font.baseSize == 0)
        return textSize;

    const int size = static_cast<int>(std::strlen(text));
    int tempByteCounter = 0;
    int byteCounter = 0;
    float textWidth = 0.0f;
    float tempTextWidth = 0.0f;
    float textHeight = fontSize;
    const float scaleFactor = fontSize / static_cast<float>(font.baseSize);

    for (int i = 0; i < size;)
    {
        ++byteCounter;

        int codepointByteCount = 0;
        const int letter = codepointNext(&text[i], &codepointByteCount);
        const int index = getGlyphIndex(font, letter);

        i += codepointByteCount;

        if (letter != '\n')
        {
            if (font.glyphs[index].advanceX > 0) textWidth += static_cast<float>(font.glyphs[index].advanceX);
            else textWidth += (font.recs[index].width + static_cast<float>(font.glyphs[index].offsetX));
        }
        else
        {
            if (tempTextWidth < textWidth) tempTextWidth = textWidth;
            byteCounter = 0;
            textWidth = 0.0f;
            textHeight += (fontSize + static_cast<float>(gTextLineSpacing));
        }

        if (tempByteCounter < byteCounter) tempByteCounter = byteCounter;
    }

    if (tempTextWidth < textWidth) tempTextWidth = textWidth;

    textSize.x = tempTextWidth * scaleFactor + static_cast<float>(tempByteCounter - 1) * spacing;
    textSize.y = textHeight;
    return textSize;
}

void DrawTextEx(Font font, const char* text, Vector2 position, float fontSize,
                float spacing, Color tint)
{
    if (font.texture.id == 0) font = GetFontDefault();
    if (font.texture.id == 0 || font.baseSize == 0 || text == nullptr) return;

    const int size = static_cast<int>(std::strlen(text));
    float textOffsetY = 0.0f;
    float textOffsetX = 0.0f;
    const float scaleFactor = fontSize / static_cast<float>(font.baseSize);

    for (int i = 0; i < size;)
    {
        int codepointByteCount = 0;
        const int codepoint = codepointNext(&text[i], &codepointByteCount);
        const int index = getGlyphIndex(font, codepoint);

        if (codepoint == '\n')
        {
            textOffsetY += (fontSize + static_cast<float>(gTextLineSpacing));
            textOffsetX = 0.0f;
        }
        else
        {
            if (codepoint != ' ' && codepoint != '\t')
            {
                // 与 raylib 的 DrawTextCodepoint 相同：目标矩形/源矩形都按 glyphPadding 外扩，
                // 并且要加上前面字符累计的 textOffsetX 与换行累计的 textOffsetY
                const Rectangle dst{
                    position.x + textOffsetX +
                        static_cast<float>(font.glyphs[index].offsetX) * scaleFactor -
                        static_cast<float>(font.glyphPadding) * scaleFactor,
                    position.y + textOffsetY +
                        static_cast<float>(font.glyphs[index].offsetY) * scaleFactor -
                        static_cast<float>(font.glyphPadding) * scaleFactor,
                    (font.recs[index].width + 2.0f * static_cast<float>(font.glyphPadding)) * scaleFactor,
                    (font.recs[index].height + 2.0f * static_cast<float>(font.glyphPadding)) * scaleFactor};
                const Rectangle src{
                    font.recs[index].x - static_cast<float>(font.glyphPadding),
                    font.recs[index].y - static_cast<float>(font.glyphPadding),
                    font.recs[index].width + 2.0f * static_cast<float>(font.glyphPadding),
                    font.recs[index].height + 2.0f * static_cast<float>(font.glyphPadding)};
                DrawTexturePro(font.texture, src, dst, Vector2{0.0f, 0.0f}, 0.0f, tint);
            }

            if (font.glyphs[index].advanceX == 0)
                textOffsetX += (font.recs[index].width * scaleFactor + spacing);
            else
                textOffsetX += (static_cast<float>(font.glyphs[index].advanceX) * scaleFactor + spacing);
        }

        i += codepointByteCount;
    }
}
