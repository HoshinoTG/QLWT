#pragma once

// ---------------------------------------------------------------------------
// gfx 基础类型与常量
//
// 这套 API 的**名字和语义**沿用 raylib（Color / Rectangle / Texture2D /
// DrawRectangleRounded / MeasureTextEx …），但实现全部换成本仓库自己的
// OpenGL 3.3 Core 渲染层 + GLFW 窗口层 + stb / miniaudio，不再链接 libraylib。
// 这样做是为了让 core/ game/ renderer/ 里上千处绘制调用点保持原样，
// 把改动集中在渲染后端，降低"换个库就把界面画歪"的风险。
//
// 与 raylib 的对应关系（迁移时对照用）：
//   raylib.h 类型            -> 这里同名的结构体
//   raylib 的 rlgl 矩阵栈     -> gfxPushMatrix / gfxScalef / gfxTranslatef
//   raylib 的 raudio（音频）  -> gfx/audio.cpp（miniaudio 实现）
//   raylib 的 rtext（字体）   -> gfx/text.cpp（stb_truetype 实现）
//   raylib 的 rtextures       -> gfx/image.cpp（stb_image 实现）
//
// 只保留工程实际用到的部分：**没有** 3D、模型、相机、VR、着色器文件加载等。
// ---------------------------------------------------------------------------

#include <cstdint>

// ---- Windows：先把和图形 API 撞名的东西关掉 ---------------------------------
// mingw 的 <GL/gl.h> 会间接 include <windows.h>，而 windows.h 里：
//   * wingdi.h 有 GDI 函数 Rectangle()      -> 和本文件的 struct Rectangle 撞名
//   * winuser.h 有宏 LoadImage / DrawTextEx -> 会把我们的同名函数改名成 ...A
//   * winmm.h  有宏 PlaySound               -> 同上
// 所以这里先定义 WIN32_LEAN_AND_MEAN / NOGDI，并在 gl_api.h 里对宏做 #undef。
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOGDI
        #define NOGDI
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif

// ---- 数学常量（与 raylib 同名，调用点不用改）-------------------------------
constexpr float PI = 3.14159265358979323846f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float RAD2DEG = 180.0f / PI;

// ---- 数学 / 绘制基础类型 ---------------------------------------------------

struct Vector2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Vector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 4x4 列主序矩阵（GL 习惯）：只在下发 mvp 用得到，这里是纯数据容器
struct Matrix
{
    float m0, m4, m8, m12;
    float m1, m5, m9, m13;
    float m2, m6, m10, m14;
    float m3, m7, m11, m15;
};

struct Color
{
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    unsigned char a = 255;
};

struct Rectangle
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// ---- 图像 / 纹理 -----------------------------------------------------------

// 像素格式：数值与 raylib 的 PixelFormat 一致（只实现用到的那几种）
enum PixelFormat
{
    PIXELFORMAT_UNCOMPRESSED_GRAYSCALE = 1,   // 8 bit / 像素（无 alpha）
    PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA,      // 16 bit / 像素（2 通道）
    PIXELFORMAT_UNCOMPRESSED_R8G8B8,          // 24 bit / 像素
    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,        // 32 bit / 像素（RGBA8）
};

// CPU 侧图像：data 由 gfx 分配，用 UnloadImage() 释放
struct Image
{
    void* data = nullptr;
    int width = 0;
    int height = 0;
    int mipmaps = 1;
    int format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
};

// GPU 侧贴图：id == 0 表示无效贴图
struct Texture2D
{
    unsigned int id = 0;
    int width = 0;
    int height = 0;
    int mipmaps = 1;
    int format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
};

// 离屏渲染目标（FBO + 颜色贴图）
//
// 开了 MSAA 时结构不一样：
//   * id         = 多采样 FBO（绘制走它，附件是多采样 renderbuffer）
//   * resolveFbo = 普通 FBO，颜色附件就是 texture
//   * EndTextureMode() 会自动把多采样缓冲 blit（解析）到 texture 里，
//     之后照常把 texture 当普通贴图采样、做后处理
struct RenderTexture2D
{
    unsigned int id = 0;        // FBO 名字（MSAA 时是多采样 FBO）
    Texture2D texture{};        // 颜色贴图（MSAA 时是解析后的结果）
    Texture2D depth{};          // 深度附件（本项目不用，占位保持结构一致）
    unsigned int msaaColor = 0; // 多采样颜色 renderbuffer（0 = 没开 MSAA）
    unsigned int resolveFbo = 0;// 解析目标 FBO（0 = 没开 MSAA，texture 直接挂在 id 上）
    int samples = 0;            // 采样数（0/1 = 关）
};

enum TextureFilter
{
    TEXTURE_FILTER_POINT = 0,            // 最近邻
    TEXTURE_FILTER_BILINEAR,             // 双线性
    TEXTURE_FILTER_TRILINEAR,            // 三线性（走 mipmap）
    TEXTURE_FILTER_ANISOTROPIC_4X,
    TEXTURE_FILTER_ANISOTROPIC_8X,
    TEXTURE_FILTER_ANISOTROPIC_16X,
};

enum TextureWrap
{
    TEXTURE_WRAP_REPEAT = 0,
    TEXTURE_WRAP_CLAMP,
    TEXTURE_WRAP_MIRROR_REPEAT,
    TEXTURE_WRAP_MIRROR_CLAMP,
};

// 混合模式（数值与 raylib 一致）
enum BlendMode
{
    BLEND_ALPHA = 0,       // 常规 alpha 混合（默认）
    BLEND_ADDITIVE,        // 加色：发光、泛光叠加用
    BLEND_MULTIPLIED,      // 正片叠底
    BLEND_ADD_COLORS,      // 直接相加（忽略 alpha）
};

// ---- 字体 ------------------------------------------------------------------

// 单个字形：offsetX/offsetY 是从绘制基准点到字形位图左上角的偏移，
// advanceX 是步进宽度（都以 fontSize 为基准，绘制时按 size/baseSize 缩放）
struct GlyphInfo
{
    int value = 0;          // 码点
    int offsetX = 0;
    int offsetY = 0;
    int advanceX = 0;
    Image image{};          // 只用于描述图集区域，不持有像素
};

struct Font
{
    int baseSize = 0;       // 字形光栅化时的字号（绘制尺寸按此缩放）
    int glyphCount = 0;
    int glyphPadding = 0;
    Texture2D texture{};    // 字形图集
    Rectangle* recs = nullptr;    // 每个字形在图集里的矩形
    GlyphInfo* glyphs = nullptr;  // 每个字形的度量

    bool valid() const { return texture.id != 0 && glyphCount > 0; }
};

// ---- 着色器 ----------------------------------------------------------------

enum ShaderUniformDataType
{
    SHADER_UNIFORM_FLOAT = 0,
    SHADER_UNIFORM_VEC2,
    SHADER_UNIFORM_VEC3,
    SHADER_UNIFORM_VEC4,
    SHADER_UNIFORM_INT,
    SHADER_UNIFORM_IVEC2,
    SHADER_UNIFORM_IVEC3,
    SHADER_UNIFORM_IVEC4,
    SHADER_UNIFORM_SAMPLER2D,
};

struct Shader
{
    unsigned int id = 0;    // GL program
    int* locs = nullptr;    // 内置 uniform 位置表（本项目只用于内部）
};

// ---- 音频 ------------------------------------------------------------------

// 音频节点句柄（miniaudio 的 ma_sound 由 gfx/audio.cpp 内部持有）
struct AudioStream
{
    void* internal = nullptr;
};

// Sound / Music 都是**句柄**：拷贝只共享底层节点，Unload 一次即可
// （和 raylib 一样，游戏代码里 `seCache[file] = s;` 这种拷贝是安全的）
struct Sound
{
    unsigned int frameCount = 0;   // 0 表示加载失败（调用方靠它判断有效性）
    AudioStream stream{};
};

struct Music
{
    unsigned int frameCount = 0;   // 0 表示加载失败
    AudioStream stream{};
    bool looping = false;
};

// ---- 颜色常量 --------------------------------------------------------------

constexpr Color LIGHTGRAY{200, 200, 200, 255};
constexpr Color GRAY{130, 130, 130, 255};
constexpr Color DARKGRAY{80, 80, 80, 255};
constexpr Color YELLOW{253, 249, 0, 255};
constexpr Color GOLD{255, 203, 0, 255};
constexpr Color ORANGE{255, 161, 0, 255};
constexpr Color PINK{255, 109, 194, 255};
constexpr Color RED{230, 41, 55, 255};
constexpr Color MAROON{190, 33, 55, 255};
constexpr Color GREEN{0, 228, 48, 255};
constexpr Color LIME{0, 158, 47, 255};
constexpr Color DARKGREEN{0, 117, 44, 255};
constexpr Color SKYBLUE{102, 191, 255, 255};
constexpr Color BLUE{0, 121, 241, 255};
constexpr Color DARKBLUE{0, 82, 172, 255};
constexpr Color PURPLE{200, 122, 255, 255};
constexpr Color VIOLET{135, 60, 190, 255};
constexpr Color DARKPURPLE{112, 31, 126, 255};
constexpr Color BEIGE{211, 176, 131, 255};
constexpr Color BROWN{127, 106, 79, 255};
constexpr Color DARKBROWN{76, 63, 47, 255};
constexpr Color WHITE{255, 255, 255, 255};
constexpr Color BLACK{0, 0, 0, 255};
constexpr Color BLANK{0, 0, 0, 0};
constexpr Color MAGENTA{255, 0, 255, 255};
constexpr Color RAYWHITE{245, 245, 245, 255};

// 按比例淡出颜色（alpha 乘 factor，钳制在 0..1）
inline Color Fade(Color color, float alpha)
{
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    Color c = color;
    c.a = static_cast<unsigned char>(static_cast<float>(color.a) * alpha);
    return c;
}

// ---- 配置标志 / 窗口状态 ---------------------------------------------------

enum ConfigFlags
{
    FLAG_VSYNC_HINT = 0x00000040,
    FLAG_FULLSCREEN_MODE = 0x00000002,
    FLAG_WINDOW_RESIZABLE = 0x00000004,
    FLAG_WINDOW_UNDECORATED = 0x00000008,
    FLAG_WINDOW_TRANSPARENT = 0x00000010,
    FLAG_WINDOW_HIDDEN = 0x00000080,
    FLAG_WINDOW_ALWAYS_RUN = 0x00000100,
    FLAG_WINDOW_MINIMIZED = 0x00000200,
    FLAG_WINDOW_MAXIMIZED = 0x00000400,
    FLAG_WINDOW_UNFOCUSED = 0x00000800,
    FLAG_WINDOW_TOPMOST = 0x00001000,
    FLAG_WINDOW_HIGHDPI = 0x00002000,
    FLAG_MSAA_4X_HINT = 0x00000020,
};

// ---- 键盘 / 鼠标 -----------------------------------------------------------
//
// KEY_* 的取值**就是 GLFW 的键码**（raylib 用的也是这一套），
// 这样输入层可以直接把 GLFW 的回调键码当数组下标用。
// window.cpp 里有 static_assert 盯着，防止哪天 GLFW 改了值而对不上。

enum KeyboardKey
{
    KEY_NULL = 0,
    KEY_APOSTROPHE = 39,
    KEY_COMMA = 44,
    KEY_MINUS = 45,
    KEY_PERIOD = 46,
    KEY_SLASH = 47,
    KEY_ZERO = 48,
    KEY_ONE = 49,
    KEY_TWO = 50,
    KEY_THREE = 51,
    KEY_FOUR = 52,
    KEY_FIVE = 53,
    KEY_SIX = 54,
    KEY_SEVEN = 55,
    KEY_EIGHT = 56,
    KEY_NINE = 57,
    KEY_SEMICOLON = 59,
    KEY_EQUAL = 61,
    KEY_A = 65,
    KEY_B = 66,
    KEY_C = 67,
    KEY_D = 68,
    KEY_E = 69,
    KEY_F = 70,
    KEY_G = 71,
    KEY_H = 72,
    KEY_I = 73,
    KEY_J = 74,
    KEY_K = 75,
    KEY_L = 76,
    KEY_M = 77,
    KEY_N = 78,
    KEY_O = 79,
    KEY_P = 80,
    KEY_Q = 81,
    KEY_R = 82,
    KEY_S = 83,
    KEY_T = 84,
    KEY_U = 85,
    KEY_V = 86,
    KEY_W = 87,
    KEY_X = 88,
    KEY_Y = 89,
    KEY_Z = 90,
    KEY_LEFT_BRACKET = 91,
    KEY_BACKSLASH = 92,
    KEY_RIGHT_BRACKET = 93,
    KEY_GRAVE = 96,
    KEY_SPACE = 32,
    KEY_ESCAPE = 256,
    KEY_ENTER = 257,
    KEY_TAB = 258,
    KEY_BACKSPACE = 259,
    KEY_INSERT = 260,
    KEY_DELETE = 261,
    KEY_RIGHT = 262,
    KEY_LEFT = 263,
    KEY_DOWN = 264,
    KEY_UP = 265,
    KEY_PAGE_UP = 266,
    KEY_PAGE_DOWN = 267,
    KEY_HOME = 268,
    KEY_END = 269,
    KEY_CAPS_LOCK = 280,
    KEY_SCROLL_LOCK = 281,
    KEY_NUM_LOCK = 282,
    KEY_PRINT_SCREEN = 283,
    KEY_PAUSE = 284,
    KEY_F1 = 290,
    KEY_F2 = 291,
    KEY_F3 = 292,
    KEY_F4 = 293,
    KEY_F5 = 294,
    KEY_F6 = 295,
    KEY_F7 = 296,
    KEY_F8 = 297,
    KEY_F9 = 298,
    KEY_F10 = 299,
    KEY_F11 = 300,
    KEY_F12 = 301,
    KEY_LEFT_SHIFT = 340,
    KEY_LEFT_CONTROL = 341,
    KEY_LEFT_ALT = 342,
    KEY_LEFT_SUPER = 343,
    KEY_RIGHT_SHIFT = 344,
    KEY_RIGHT_CONTROL = 345,
    KEY_RIGHT_ALT = 346,
    KEY_RIGHT_SUPER = 347,
    KEY_KB_MENU = 348,
    KEY_KP_0 = 320,
    KEY_KP_1 = 321,
    KEY_KP_2 = 322,
    KEY_KP_3 = 323,
    KEY_KP_4 = 324,
    KEY_KP_5 = 325,
    KEY_KP_6 = 326,
    KEY_KP_7 = 327,
    KEY_KP_8 = 328,
    KEY_KP_9 = 329,
    KEY_KP_DECIMAL = 330,
    KEY_KP_DIVIDE = 331,
    KEY_KP_MULTIPLY = 332,
    KEY_KP_SUBTRACT = 333,
    KEY_KP_ADD = 334,
    KEY_KP_ENTER = 335,
    KEY_KP_EQUAL = 336,
};

enum MouseButton
{
    MOUSE_BUTTON_LEFT = 0,
    MOUSE_BUTTON_RIGHT = 1,
    MOUSE_BUTTON_MIDDLE = 2,
    MOUSE_BUTTON_SIDE = 3,
    MOUSE_BUTTON_EXTRA = 4,
    MOUSE_BUTTON_FORWARD = 5,
    MOUSE_BUTTON_BACK = 6,
};
