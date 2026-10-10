#include "gfx.h"

#include "gfx_internal.h"
#include "gl_api.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>

// ---------------------------------------------------------------------------
// 窗口 / 输入 / 帧计时（GLFW + OpenGL 3.3 Core）
//
// 这一层对应 raylib 的 rcore + rcore_desktop_glfw：
//   * 建窗口、建 GL 上下文、状态标志（可缩放 / 全屏 / VSync / HiDPI）
//   * 键盘鼠标状态机：IsKeyPressed 判断的是"这一帧刚按下"，
//     所以每帧末把 current 拷进 previous，再收事件（和 raylib 一致）
//   * 帧计时：GetFrameTime 返回上一帧的总耗时（update + draw + 等帧）
//
// HiDPI：逻辑窗口尺寸（GetScreenWidth）与帧缓冲像素尺寸（GetRenderWidth）分开。
// 投影用逻辑尺寸、视口用帧缓冲尺寸，于是 150% 缩放的屏幕上画出来是清晰的，
// 而不是被合成器放大糊掉。
// ---------------------------------------------------------------------------

namespace
{
constexpr int kMaxKeys = 512;
constexpr int kMaxMouseButtons = 8;

struct State
{
    GLFWwindow* window = nullptr;
    bool ready = false;
    bool shouldClose = false;

    unsigned int configFlags = 0;   // SetConfigFlags 传入
    unsigned int windowFlags = 0;   // 运行时状态（全屏等）
    int exitKey = KEY_ESCAPE;

    // 窗口位置（进出全屏时恢复用）
    int windowedX = 0;
    int windowedY = 0;

    // 帧计时
    double timePrevious = 0.0;
    double frameTime = 1.0 / 60.0;
    double updateTime = 0.0;
    double targetFrameTime = 0.0;   // 0 = 不限帧

    // 输入
    bool keyCurrent[kMaxKeys] = {};
    bool keyPrevious[kMaxKeys] = {};
    bool mouseCurrent[kMaxMouseButtons] = {};
    bool mousePrevious[kMaxMouseButtons] = {};
    Vector2 mousePosition{};
    Vector2 wheel{};
    int mouseOffsetX = 0;
    int mouseOffsetY = 0;
    float mouseScaleX = 1.0f;
    float mouseScaleY = 1.0f;
};

State g;

void errorCallback(int code, const char* description)
{
    std::fprintf(stderr, "[gfx] GLFW 错误 %d：%s\n", code, description);
}

// ---- 事件回调 --------------------------------------------------------------

void keyCallback(GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int /*mods*/)
{
    if (key < 0 || key >= kMaxKeys) return;
    if (action == GLFW_RELEASE) g.keyCurrent[key] = false;
    else g.keyCurrent[key] = true;   // PRESS 与 REPEAT 都算按住

    if (action == GLFW_PRESS && key == g.exitKey) g.shouldClose = true;
}

void mouseButtonCallback(GLFWwindow* /*window*/, int button, int action, int /*mods*/)
{
    if (button < 0 || button >= kMaxMouseButtons) return;
    g.mouseCurrent[button] = (action == GLFW_PRESS);
}

void cursorPosCallback(GLFWwindow* /*window*/, double x, double y)
{
    g.mousePosition.x = static_cast<float>(x);
    g.mousePosition.y = static_cast<float>(y);
}

void scrollCallback(GLFWwindow* /*window*/, double xoffset, double yoffset)
{
    g.wheel.x = static_cast<float>(xoffset);
    g.wheel.y = static_cast<float>(yoffset);
}

void closeCallback(GLFWwindow* /*window*/) { g.shouldClose = true; }

// 收事件：先把本帧状态记为"上一帧"，再让 GLFW 派发新事件
void pollEvents()
{
    for (int i = 0; i < kMaxKeys; ++i) g.keyPrevious[i] = g.keyCurrent[i];
    for (int i = 0; i < kMaxMouseButtons; ++i) g.mousePrevious[i] = g.mouseCurrent[i];
    g.wheel = Vector2{};
    glfwPollEvents();
}
}   // namespace

// ---------------------------------------------------------------------------
// 内部接口（给 batch.cpp 用）
// ---------------------------------------------------------------------------

namespace gfxdetail
{
bool hasWindow() { return g.window != nullptr; }

void framebufferSize(int* width, int* height)
{
    int w = 0, h = 0;
    if (g.window != nullptr) glfwGetFramebufferSize(g.window, &w, &h);
    if (width != nullptr) *width = w;
    if (height != nullptr) *height = h;
}

void beginFrameTiming()
{
    const double now = glfwGetTime();
    g.updateTime = now - g.timePrevious;
    g.timePrevious = now;
}

void swapAndEndFrame()
{
    if (g.window == nullptr) return;

    glfwSwapBuffers(g.window);   // VSync 打开时这里会阻塞到刷新

    double now = glfwGetTime();
    g.frameTime = g.updateTime + (now - g.timePrevious);
    g.timePrevious = now;

    // 限帧：把这一帧剩余的时间睡掉
    if (g.targetFrameTime > 0.0 && g.frameTime < g.targetFrameTime)
    {
        const double remaining = g.targetFrameTime - g.frameTime;
        std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
        now = glfwGetTime();
        g.frameTime += now - g.timePrevious;
        g.timePrevious = now;
    }

    pollEvents();
}
}   // namespace gfxdetail

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------

void SetConfigFlags(unsigned int flags) { g.configFlags = flags; }

void InitWindow(int width, int height, const char* title)
{
    glfwSetErrorCallback(errorCallback);
    if (!glfwInit())
    {
        std::fprintf(stderr, "[gfx] glfwInit 失败\n");
        return;
    }

    // 输入状态数组的取值必须和 GLFW 的键码一致（gfx/types.h 里就是这么定义的）
    static_assert(KEY_ESCAPE == GLFW_KEY_ESCAPE, "KEY_* 必须与 GLFW 键码一致");
    static_assert(KEY_ENTER == GLFW_KEY_ENTER, "KEY_* 必须与 GLFW 键码一致");
    static_assert(KEY_SPACE == GLFW_KEY_SPACE, "KEY_* 必须与 GLFW 键码一致");
    static_assert(KEY_UP == GLFW_KEY_UP && KEY_LEFT == GLFW_KEY_LEFT, "KEY_* 必须与 GLFW 键码一致");
    static_assert(KEY_LEFT_CONTROL == GLFW_KEY_LEFT_CONTROL, "KEY_* 必须与 GLFW 键码一致");
    static_assert(MOUSE_BUTTON_LEFT == GLFW_MOUSE_BUTTON_LEFT, "MOUSE_BUTTON_* 必须与 GLFW 一致");

    glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#if defined(GLFW_SCALE_FRAMEBUFFER)
    // Wayland 等平台上让帧缓冲跟着显示缩放走（150% 缩放的屏幕才有 HiDPI 清晰度）
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, (g.configFlags & FLAG_WINDOW_RESIZABLE) ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, (g.configFlags & FLAG_WINDOW_HIDDEN) ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, (g.configFlags & FLAG_WINDOW_UNDECORATED) ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, (g.configFlags & FLAG_WINDOW_TOPMOST) ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER,
                   (g.configFlags & FLAG_WINDOW_TRANSPARENT) ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES, (g.configFlags & FLAG_MSAA_4X_HINT) ? 4 : 0);
    // 窗口不透明时不要 alpha 通道：省一点带宽，也让截图里的 alpha 恒为 255
    // （需要透明窗口时才申请 8 位 alpha）
    glfwWindowHint(GLFW_ALPHA_BITS, (g.configFlags & FLAG_WINDOW_TRANSPARENT) ? 8 : 0);

    g.window = glfwCreateWindow(width, height, title != nullptr ? title : "qlwt", nullptr, nullptr);
    if (g.window == nullptr)
    {
        std::fprintf(stderr, "[gfx] 创建窗口失败（需要 OpenGL 3.3 Core）\n");
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(g.window);
    glfwSwapInterval((g.configFlags & FLAG_VSYNC_HINT) ? 1 : 0);

    const char* missing = gfxLoadOpenGL();
    if (missing != nullptr)
    {
        std::fprintf(stderr, "[gfx] 当前驱动缺少 OpenGL 3.3 入口：%s\n", missing);
        glfwDestroyWindow(g.window);
        g.window = nullptr;
        glfwTerminate();
        return;
    }

    glfwSetKeyCallback(g.window, keyCallback);
    glfwSetMouseButtonCallback(g.window, mouseButtonCallback);
    glfwSetCursorPosCallback(g.window, cursorPosCallback);
    glfwSetScrollCallback(g.window, scrollCallback);
    glfwSetWindowCloseCallback(g.window, closeCallback);

    // 初始鼠标位置：GLFW 只在移动时回调，先主动读一次
    {
        double mx = 0.0, my = 0.0;
        glfwGetCursorPos(g.window, &mx, &my);
        g.mousePosition = {static_cast<float>(mx), static_cast<float>(my)};
    }

    gfxdetail::initBatch();

    g.windowFlags = (g.configFlags & FLAG_WINDOW_RESIZABLE);
    g.timePrevious = glfwGetTime();
    g.targetFrameTime = 1.0 / 60.0;
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    g.ready = true;

    int fbW = 0, fbH = 0;
    gfxdetail::framebufferSize(&fbW, &fbH);
    std::printf("[gfx] OpenGL %s | %s | 窗口 %dx%d | 帧缓冲 %dx%d\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                GetScreenWidth(), GetScreenHeight(), fbW, fbH);
}

bool IsWindowReady() { return g.ready; }

void CloseWindow()
{
    if (g.window != nullptr)
    {
        glfwDestroyWindow(g.window);
        g.window = nullptr;
    }
    glfwTerminate();
    g.ready = false;
}

bool WindowShouldClose()
{
    if (g.window == nullptr) return true;
    return g.shouldClose || glfwWindowShouldClose(g.window) == GLFW_TRUE;
}

void SetWindowMinSize(int width, int height)
{
    if (g.window == nullptr) return;
    glfwSetWindowSizeLimits(g.window, width, height, GLFW_DONT_CARE, GLFW_DONT_CARE);
}

void ToggleFullscreen()
{
    if (g.window == nullptr) return;

    if ((g.windowFlags & FLAG_FULLSCREEN_MODE) == 0)
    {
        glfwGetWindowPos(g.window, &g.windowedX, &g.windowedY);
        GLFWmonitor* monitor = glfwGetWindowMonitor(g.window);
        if (monitor == nullptr) monitor = glfwGetPrimaryMonitor();
        if (monitor == nullptr) return;
        glfwSetWindowMonitor(g.window, monitor, 0, 0, GetScreenWidth(), GetScreenHeight(),
                            GLFW_DONT_CARE);
        g.windowFlags |= FLAG_FULLSCREEN_MODE;
    }
    else
    {
        glfwSetWindowMonitor(g.window, nullptr, g.windowedX, g.windowedY,
                            GetScreenWidth(), GetScreenHeight(), GLFW_DONT_CARE);
        g.windowFlags &= ~FLAG_FULLSCREEN_MODE;
    }

    if (g.configFlags & FLAG_VSYNC_HINT) glfwSwapInterval(1);
}

void SetWindowState(unsigned int flags)
{
    if (g.window == nullptr) return;

    if ((flags & FLAG_VSYNC_HINT) != 0 && (g.windowFlags & FLAG_VSYNC_HINT) == 0)
    {
        glfwSwapInterval(1);
        g.windowFlags |= FLAG_VSYNC_HINT;
    }
    if ((flags & FLAG_FULLSCREEN_MODE) != 0 && (g.windowFlags & FLAG_FULLSCREEN_MODE) == 0)
    {
        ToggleFullscreen();
    }
    if ((flags & FLAG_WINDOW_RESIZABLE) != 0 && (g.windowFlags & FLAG_WINDOW_RESIZABLE) == 0)
    {
        glfwSetWindowAttrib(g.window, GLFW_RESIZABLE, GLFW_TRUE);
        g.windowFlags |= FLAG_WINDOW_RESIZABLE;
    }
}

void ClearWindowState(unsigned int flags)
{
    if (g.window == nullptr) return;
    if ((flags & FLAG_FULLSCREEN_MODE) != 0 && (g.windowFlags & FLAG_FULLSCREEN_MODE) != 0)
        ToggleFullscreen();
    if ((flags & FLAG_VSYNC_HINT) != 0 && (g.windowFlags & FLAG_VSYNC_HINT) != 0)
    {
        glfwSwapInterval(0);
        g.windowFlags &= ~FLAG_VSYNC_HINT;
    }
    g.windowFlags &= ~flags;
}

bool IsWindowState(unsigned int flags) { return (g.windowFlags & flags) != 0; }

void SetExitKey(int key) { g.exitKey = key; }

void SetTargetFPS(int fps)
{
    g.targetFrameTime = (fps < 1) ? 0.0 : 1.0 / static_cast<double>(fps);
}

int GetScreenWidth()
{
    int w = 0, h = 0;
    if (g.window != nullptr) glfwGetWindowSize(g.window, &w, &h);
    return w;
}

int GetScreenHeight()
{
    int w = 0, h = 0;
    if (g.window != nullptr) glfwGetWindowSize(g.window, &w, &h);
    return h;
}

int GetRenderWidth()
{
    int w = 0, h = 0;
    gfxdetail::framebufferSize(&w, &h);
    return w;
}

int GetRenderHeight()
{
    int w = 0, h = 0;
    gfxdetail::framebufferSize(&w, &h);
    return h;
}

float GetFrameTime() { return static_cast<float>(g.frameTime); }

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------

bool IsKeyDown(int key)
{
    if (key < 0 || key >= kMaxKeys) return false;
    return g.keyCurrent[key];
}

bool IsKeyPressed(int key)
{
    if (key < 0 || key >= kMaxKeys) return false;
    return g.keyCurrent[key] && !g.keyPrevious[key];
}

bool IsKeyReleased(int key)
{
    if (key < 0 || key >= kMaxKeys) return false;
    return !g.keyCurrent[key] && g.keyPrevious[key];
}

bool IsMouseButtonDown(int button)
{
    if (button < 0 || button >= kMaxMouseButtons) return false;
    return g.mouseCurrent[button];
}

bool IsMouseButtonPressed(int button)
{
    if (button < 0 || button >= kMaxMouseButtons) return false;
    return g.mouseCurrent[button] && !g.mousePrevious[button];
}

bool IsMouseButtonReleased(int button)
{
    if (button < 0 || button >= kMaxMouseButtons) return false;
    return !g.mouseCurrent[button] && g.mousePrevious[button];
}

void SetMouseOffset(int offsetX, int offsetY)
{
    g.mouseOffsetX = offsetX;
    g.mouseOffsetY = offsetY;
}

void SetMouseScale(float scaleX, float scaleY)
{
    g.mouseScaleX = scaleX;
    g.mouseScaleY = scaleY;
}

Vector2 GetMousePosition()
{
    // 与 raylib 相同的映射：(原始坐标 + offset) * scale
    return Vector2{(g.mousePosition.x + static_cast<float>(g.mouseOffsetX)) * g.mouseScaleX,
                   (g.mousePosition.y + static_cast<float>(g.mouseOffsetY)) * g.mouseScaleY};
}

int GetMouseX() { return static_cast<int>(GetMousePosition().x); }

float GetMouseWheelMove() { return g.wheel.y; }

Vector2 GetMouseWheelMoveV() { return g.wheel; }

// ---------------------------------------------------------------------------
// 杂项
// ---------------------------------------------------------------------------

bool FileExists(const char* fileName)
{
    if (fileName == nullptr) return false;
    std::error_code ec;
    return std::filesystem::exists(fileName, ec);
}

int GetRandomValue(int min, int max)
{
    if (min > max)
    {
        const int tmp = max;
        max = min;
        min = tmp;
    }
    if (min == max) return min;
    return std::rand() % (std::abs(max - min) + 1) + min;
}
