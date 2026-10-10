#include "gfx.h"

#include "gfx_internal.h"
#include "gl_api.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 图片与纹理（stb_image / stb_image_write / stb_image_resize2）
//
// 对应 raylib 的 rtextures：
//   * LoadImage / LoadImageAnim(GIF) 解码到 CPU 侧 Image
//   * ImageResize / ImageBlurGaussian 等在 CPU 上做像素处理
//   * LoadTextureFromImage / UpdateTexture 上传到 GPU
//   * LoadImageFromScreen / TakeScreenshot 从帧缓冲取像
//
// 三种 stb 库的实现在这里展开（全工程只此一处），头文件本身在 third_party/。
// ---------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wcast-qual"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"
#include "../third_party/stb_image_resize2.h"
#pragma GCC diagnostic pop

namespace
{
// raylib 的 ImageBlurGaussian 用 4 次盒式模糊逼近高斯
constexpr int kGaussianBlurIterations = 4;

int bytesPerPixel(int format)
{
    switch (format)
    {
    case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE: return 1;
    case PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA: return 2;
    case PIXELFORMAT_UNCOMPRESSED_R8G8B8: return 3;
    case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8: return 4;
    default: return 0;
    }
}

void glFormatOf(int format, unsigned int* glInternal, unsigned int* glFormat)
{
    switch (format)
    {
    case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE:
        *glInternal = GL_R8;
        *glFormat = GL_RED;
        break;
    case PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA:
        *glInternal = GL_RG8;
        *glFormat = GL_RG;
        break;
    case PIXELFORMAT_UNCOMPRESSED_R8G8B8:
        *glInternal = GL_RGB8;
        *glFormat = GL_RGB;
        break;
    case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8:
    default:
        *glInternal = GL_RGBA8;
        *glFormat = GL_RGBA;
        break;
    }
}

std::vector<unsigned char> readFile(const char* fileName)
{
    std::vector<unsigned char> data;
    std::ifstream file(fileName, std::ios::binary | std::ios::ate);
    if (!file) return data;
    const std::streamsize size = file.tellg();
    if (size <= 0) return data;
    file.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool endsWithIgnoreCase(const std::string& text, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
    {
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(text[text.size() - n + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}
}   // namespace

// ---------------------------------------------------------------------------
// 贴图
// ---------------------------------------------------------------------------

Texture2D LoadTextureFromImage(Image image)
{
    Texture2D texture{};
    if (image.data == nullptr || image.width <= 0 || image.height <= 0) return texture;

    unsigned int glInternal = GL_RGBA8;
    unsigned int glFormat = GL_RGBA;
    glFormatOf(image.format, &glInternal, &glFormat);

    glGenTextures(1, &texture.id);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(glInternal), image.width, image.height, 0,
                 glFormat, GL_UNSIGNED_BYTE, image.data);

    // 默认与 raylib 一致：最近邻 + 重复
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // 灰度图只上传单通道，用 swizzle 把 R 复制到 RGB 上：
    // 否则默认着色器采出来是 (r,0,0) —— 字形会变成红色的。
    // 这和 raylib 的 rlLoadTexture 处理方式一致。
    if (image.format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE)
    {
        const GLint swizzle[4] = {GL_RED, GL_RED, GL_RED, GL_ONE};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    }
    else if (image.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA)
    {
        const GLint swizzle[4] = {GL_RED, GL_RED, GL_RED, GL_GREEN};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    texture.width = image.width;
    texture.height = image.height;
    texture.mipmaps = 1;
    texture.format = image.format;
    return texture;
}

Texture2D LoadTexture(const char* fileName)
{
    Image image = LoadImage(fileName);
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    return texture;
}

void UnloadTexture(Texture2D texture)
{
    if (texture.id == 0) return;
    glDeleteTextures(1, &texture.id);
}

void UpdateTexture(Texture2D texture, const void* pixels)
{
    if (texture.id == 0 || pixels == nullptr) return;
    unsigned int glInternal = GL_RGBA8;
    unsigned int glFormat = GL_RGBA;
    glFormatOf(texture.format, &glInternal, &glFormat);

    glBindTexture(GL_TEXTURE_2D, texture.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height,
                    glFormat, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SetTextureFilter(Texture2D texture, int filter)
{
    if (texture.id == 0) return;

    int minFilter = GL_NEAREST;
    int magFilter = GL_NEAREST;
    switch (filter)
    {
    case TEXTURE_FILTER_BILINEAR:
        minFilter = GL_LINEAR;
        magFilter = GL_LINEAR;
        break;
    case TEXTURE_FILTER_TRILINEAR:
    case TEXTURE_FILTER_ANISOTROPIC_4X:
    case TEXTURE_FILTER_ANISOTROPIC_8X:
    case TEXTURE_FILTER_ANISOTROPIC_16X:
        minFilter = GL_LINEAR_MIPMAP_LINEAR;
        magFilter = GL_LINEAR;
        break;
    case TEXTURE_FILTER_POINT:
    default:
        break;
    }

    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SetTextureWrap(Texture2D texture, int wrap)
{
    if (texture.id == 0) return;

    int glWrap = GL_REPEAT;
    switch (wrap)
    {
    case TEXTURE_WRAP_CLAMP: glWrap = GL_CLAMP_TO_EDGE; break;
    case TEXTURE_WRAP_MIRROR_REPEAT: glWrap = GL_MIRRORED_REPEAT; break;
    case TEXTURE_WRAP_MIRROR_CLAMP: glWrap = GL_MIRROR_CLAMP_TO_EDGE; break;
    case TEXTURE_WRAP_REPEAT:
    default: break;
    }

    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrap);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GenTextureMipmaps(Texture2D* texture)
{
    if (texture == nullptr || texture->id == 0) return;
    glBindTexture(GL_TEXTURE_2D, texture->id);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 记录层数（和 raylib 一致：1 + floor(log2(max(w,h)))），只是个信息字段
    const int maxSide = texture->width > texture->height ? texture->width : texture->height;
    int levels = 1;
    while ((1 << levels) <= maxSide) ++levels;
    texture->mipmaps = levels;
}

// ---------------------------------------------------------------------------
// 图像
// ---------------------------------------------------------------------------

Image LoadImage(const char* fileName)
{
    Image image{};
    if (fileName == nullptr) return image;

    int width = 0, height = 0, comp = 0;
    // 统一取 4 通道，格式固定 RGBA8（和 raylib 的 LoadImage 行为一致）
    unsigned char* pixels = stbi_load(fileName, &width, &height, &comp, 4);
    if (pixels == nullptr)
    {
        std::fprintf(stderr, "[gfx] 图片解码失败：%s\n", fileName);
        return image;
    }

    image.data = pixels;
    image.width = width;
    image.height = height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return image;
}

Image LoadImageAnim(const char* fileName, int* frames)
{
    Image image{};
    int frameCount = 1;
    if (fileName == nullptr)
    {
        if (frames != nullptr) *frames = 0;
        return image;
    }

    const std::string path(fileName);
    if (endsWithIgnoreCase(path, ".gif"))
    {
        // stb_image 的 GIF 解码：所有帧竖着叠在一张图上（和 raylib 相同）
        std::vector<unsigned char> data = readFile(fileName);
        if (!data.empty())
        {
            int width = 0, height = 0, comp = 0;
            int* delays = nullptr;
            unsigned char* pixels = stbi_load_gif_from_memory(
                data.data(), static_cast<int>(data.size()), &delays, &width, &height,
                &frameCount, &comp, 4);
            if (delays != nullptr) STBI_FREE(delays);

            if (pixels != nullptr)
            {
                image.data = pixels;
                image.width = width;
                image.height = height;
                image.mipmaps = 1;
                image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                if (frames != nullptr) *frames = frameCount;
                return image;
            }
        }
    }

    image = LoadImage(fileName);
    if (frames != nullptr) *frames = image.data != nullptr ? 1 : 0;
    return image;
}

Image ImageFromImage(Image image, Rectangle rec)
{
    Image result{};
    const int bpp = bytesPerPixel(image.format);
    if (image.data == nullptr || bpp == 0) return result;

    int x0 = static_cast<int>(rec.x);
    int y0 = static_cast<int>(rec.y);
    int w = static_cast<int>(rec.width);
    int h = static_cast<int>(rec.height);

    // 裁剪到图像范围内
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > image.width) w = image.width - x0;
    if (y0 + h > image.height) h = image.height - y0;
    if (w <= 0 || h <= 0) return result;

    const size_t rowBytes = static_cast<size_t>(w) * static_cast<size_t>(bpp);
    unsigned char* pixels = static_cast<unsigned char*>(std::malloc(rowBytes * static_cast<size_t>(h)));
    if (pixels == nullptr) return result;

    const unsigned char* src = static_cast<const unsigned char*>(image.data);
    for (int row = 0; row < h; ++row)
    {
        const size_t srcOffset =
            (static_cast<size_t>(y0 + row) * static_cast<size_t>(image.width) +
             static_cast<size_t>(x0)) * static_cast<size_t>(bpp);
        std::memcpy(pixels + static_cast<size_t>(row) * rowBytes, src + srcOffset, rowBytes);
    }

    result.data = pixels;
    result.width = w;
    result.height = h;
    result.mipmaps = 1;
    result.format = image.format;
    return result;
}

void UnloadImage(Image image)
{
    if (image.data != nullptr) std::free(image.data);
}

Color* LoadImageColors(Image image)
{
    if (image.data == nullptr || image.width <= 0 || image.height <= 0) return nullptr;

    const int count = image.width * image.height;
    Color* colors = static_cast<Color*>(std::malloc(sizeof(Color) * static_cast<size_t>(count)));
    if (colors == nullptr) return nullptr;

    const unsigned char* src = static_cast<const unsigned char*>(image.data);
    for (int i = 0; i < count; ++i)
    {
        switch (image.format)
        {
        case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE:
            colors[i] = Color{src[i], src[i], src[i], 255};
            break;
        case PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA:
            colors[i] = Color{src[i * 2], src[i * 2], src[i * 2], src[i * 2 + 1]};
            break;
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8:
            colors[i] = Color{src[i * 3], src[i * 3 + 1], src[i * 3 + 2], 255};
            break;
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8:
        default:
            colors[i] = Color{src[i * 4], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]};
            break;
        }
    }
    return colors;
}

void UnloadImageColors(Color* colors)
{
    if (colors != nullptr) std::free(colors);
}

void ImageResize(Image* image, int newWidth, int newHeight)
{
    if (image == nullptr || image->data == nullptr || image->width == 0 || image->height == 0) return;
    if (newWidth <= 0 || newHeight <= 0) return;

    const int bpp = bytesPerPixel(image->format);
    if (bpp == 0) return;

    const size_t size = static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight) *
                        static_cast<size_t>(bpp);
    unsigned char* output = static_cast<unsigned char*>(std::malloc(size));
    if (output == nullptr) return;

    stbir_resize_uint8_linear(static_cast<const unsigned char*>(image->data), image->width,
                              image->height, 0, output, newWidth, newHeight, 0,
                              static_cast<stbir_pixel_layout>(bpp));

    std::free(image->data);
    image->data = output;
    image->width = newWidth;
    image->height = newHeight;
}

void ImageBlurGaussian(Image* image, int blurSize)
{
    if (image == nullptr || image->data == nullptr || image->width == 0 || image->height == 0) return;
    if (blurSize <= 0) return;

    // 只处理 RGBA8：本项目所有图片（含截图、GIF 帧）都是这个格式
    if (image->format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    {
        std::fprintf(stderr, "[gfx] ImageBlurGaussian 只支持 RGBA8（当前格式 %d）\n", image->format);
        return;
    }

    const int width = image->width;
    const int height = image->height;
    unsigned char* data = static_cast<unsigned char*>(image->data);

    // 先做 alpha 预乘，避免透明像素的黑色渗进模糊结果
    for (int i = 0; i < width * height; ++i)
    {
        unsigned char* px = data + static_cast<size_t>(i) * 4;
        if (px[3] == 0)
        {
            px[0] = px[1] = px[2] = 0;
        }
        else if (px[3] < 255)
        {
            const float alpha = static_cast<float>(px[3]) / 255.0f;
            px[0] = static_cast<unsigned char>(static_cast<float>(px[0]) * alpha);
            px[1] = static_cast<unsigned char>(static_cast<float>(px[1]) * alpha);
            px[2] = static_cast<unsigned char>(static_cast<float>(px[2]) * alpha);
        }
    }

    // 用 float 缓冲做多轮盒式模糊（多轮盒式模糊逼近高斯）
    struct Pixel4
    {
        float x, y, z, w;
    };
    std::vector<Pixel4> bufferA(static_cast<size_t>(width) * static_cast<size_t>(height));
    std::vector<Pixel4> bufferB(static_cast<size_t>(width) * static_cast<size_t>(height));

    for (int i = 0; i < width * height; ++i)
    {
        bufferA[static_cast<size_t>(i)] = Pixel4{
            static_cast<float>(data[i * 4 + 0]), static_cast<float>(data[i * 4 + 1]),
            static_cast<float>(data[i * 4 + 2]), static_cast<float>(data[i * 4 + 3])};
    }

    for (int iteration = 0; iteration < kGaussianBlurIterations; ++iteration)
    {
        // 横向
        for (int row = 0; row < height; ++row)
        {
            float avgR = 0.0f, avgG = 0.0f, avgB = 0.0f, avgA = 0.0f;
            int convolutionSize = blurSize;
            for (int i = 0; i < blurSize; ++i)
            {
                const Pixel4& p = bufferA[static_cast<size_t>(row) * width + i];
                avgR += p.x;
                avgG += p.y;
                avgB += p.z;
                avgA += p.w;
            }
            for (int x = 0; x < width; ++x)
            {
                if (x - blurSize - 1 >= 0)
                {
                    const Pixel4& p = bufferA[static_cast<size_t>(row) * width + x - blurSize - 1];
                    avgR -= p.x;
                    avgG -= p.y;
                    avgB -= p.z;
                    avgA -= p.w;
                    --convolutionSize;
                }
                if (x + blurSize < width)
                {
                    const Pixel4& p = bufferA[static_cast<size_t>(row) * width + x + blurSize];
                    avgR += p.x;
                    avgG += p.y;
                    avgB += p.z;
                    avgA += p.w;
                    ++convolutionSize;
                }
                Pixel4& out = bufferB[static_cast<size_t>(row) * width + x];
                out.x = avgR / static_cast<float>(convolutionSize);
                out.y = avgG / static_cast<float>(convolutionSize);
                out.z = avgB / static_cast<float>(convolutionSize);
                out.w = avgA / static_cast<float>(convolutionSize);
            }
        }

        // 纵向
        for (int col = 0; col < width; ++col)
        {
            float avgR = 0.0f, avgG = 0.0f, avgB = 0.0f, avgA = 0.0f;
            int convolutionSize = blurSize;
            for (int i = 0; i < blurSize; ++i)
            {
                const Pixel4& p = bufferB[static_cast<size_t>(i) * width + col];
                avgR += p.x;
                avgG += p.y;
                avgB += p.z;
                avgA += p.w;
            }
            for (int y = 0; y < height; ++y)
            {
                if (y - blurSize - 1 >= 0)
                {
                    const Pixel4& p = bufferB[static_cast<size_t>(y - blurSize - 1) * width + col];
                    avgR -= p.x;
                    avgG -= p.y;
                    avgB -= p.z;
                    avgA -= p.w;
                    --convolutionSize;
                }
                if (y + blurSize < height)
                {
                    const Pixel4& p = bufferB[static_cast<size_t>(y + blurSize) * width + col];
                    avgR += p.x;
                    avgG += p.y;
                    avgB += p.z;
                    avgA += p.w;
                    ++convolutionSize;
                }
                Pixel4& out = bufferA[static_cast<size_t>(y) * width + col];
                out.x = static_cast<float>(static_cast<unsigned char>(avgR / static_cast<float>(convolutionSize)));
                out.y = static_cast<float>(static_cast<unsigned char>(avgG / static_cast<float>(convolutionSize)));
                out.z = static_cast<float>(static_cast<unsigned char>(avgB / static_cast<float>(convolutionSize)));
                out.w = static_cast<float>(static_cast<unsigned char>(avgA / static_cast<float>(convolutionSize)));
            }
        }
    }

    // 反预乘
    for (int i = 0; i < width * height; ++i)
    {
        const Pixel4& p = bufferA[static_cast<size_t>(i)];
        unsigned char* px = data + static_cast<size_t>(i) * 4;
        if (p.w == 0.0f)
        {
            px[0] = px[1] = px[2] = px[3] = 0;
        }
        else if (p.w <= 255.0f)
        {
            const float alpha = p.w / 255.0f;
            px[0] = static_cast<unsigned char>(p.x / alpha);
            px[1] = static_cast<unsigned char>(p.y / alpha);
            px[2] = static_cast<unsigned char>(p.z / alpha);
            px[3] = static_cast<unsigned char>(p.w);
        }
    }
}

bool ExportImage(Image image, const char* fileName)
{
    if (image.data == nullptr || fileName == nullptr) return false;

    // 非 RGBA8 先转成 RGBA8 再写 PNG
    if (image.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    {
        return stbi_write_png(fileName, image.width, image.height, 4, image.data,
                              image.width * 4) != 0;
    }

    Color* colors = LoadImageColors(image);
    if (colors == nullptr) return false;
    const int ok = stbi_write_png(fileName, image.width, image.height, 4, colors,
                                  image.width * 4);
    UnloadImageColors(colors);
    return ok != 0;
}

Image LoadImageFromScreen()
{
    Image image{};

    const int width = gfxdetail::currentRenderTargetWidth();
    const int height = gfxdetail::currentRenderTargetHeight();
    if (width <= 0 || height <= 0) return image;

    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
    if (pixels == nullptr) return image;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // glReadPixels 的原点在左下角：上下翻转一行一行拷成"第一行在上"
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    unsigned char* flipped = static_cast<unsigned char*>(std::malloc(size));
    if (flipped == nullptr)
    {
        std::free(pixels);
        return image;
    }
    for (int row = 0; row < height; ++row)
    {
        std::memcpy(flipped + static_cast<size_t>(row) * rowBytes,
                    pixels + static_cast<size_t>(height - 1 - row) * rowBytes, rowBytes);
    }
    std::free(pixels);

    image.data = flipped;
    image.width = width;
    image.height = height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return image;
}

void TakeScreenshot(const char* fileName)
{
    if (fileName == nullptr) return;
    Image image = LoadImageFromScreen();
    if (image.data == nullptr) return;
    ExportImage(image, fileName);
    std::printf("[gfx] 截图已保存：%s (%dx%d)\n", fileName, image.width, image.height);
    UnloadImage(image);
}
