// 输入状态机自检：直接喂事件给 gfx 的内部回调（把 window.cpp 包含进来取内部符号），
// 验证"刚按下 / 按住 / 刚松开 / 滚轮 / 鼠标坐标映射 / 退出键"这套逻辑。
#include "../gfx/window.cpp"

#include <cstdio>

static int failures = 0;
static void check(const char* what, bool ok)
{
    std::printf("  %-46s %s\n", what, ok ? "✓" : "✗");
    if (!ok) ++failures;
}

int main()
{
    std::printf("== 键盘 ==\n");
    check("初始：没按下", !IsKeyDown(KEY_SPACE) && !IsKeyPressed(KEY_SPACE));
    keyCallback(nullptr, KEY_SPACE, 0, GLFW_PRESS, 0);
    check("按下后：IsKeyDown", IsKeyDown(KEY_SPACE));
    check("按下后：IsKeyPressed（本帧刚按下）", IsKeyPressed(KEY_SPACE));
    pollEvents();
    check("下一帧：IsKeyDown 仍为真", IsKeyDown(KEY_SPACE));
    check("下一帧：IsKeyPressed 变假（不会重复触发）", !IsKeyPressed(KEY_SPACE));
    keyCallback(nullptr, KEY_SPACE, 0, GLFW_REPEAT, 0);
    pollEvents();
    check("自动重复：IsKeyPressed 不触发", !IsKeyPressed(KEY_SPACE) && IsKeyDown(KEY_SPACE));
    keyCallback(nullptr, KEY_SPACE, 0, GLFW_RELEASE, 0);
    check("松开后：IsKeyReleased", IsKeyReleased(KEY_SPACE));
    pollEvents();
    check("下一帧：IsKeyReleased 变假", !IsKeyReleased(KEY_SPACE));

    std::printf("== 鼠标按键 ==\n");
    mouseButtonCallback(nullptr, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    check("左键按下：IsMouseButtonPressed", IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
    check("左键按下：右键不受影响", !IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
    pollEvents();
    check("下一帧：按下变假 / 按住仍真", !IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                                          IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    mouseButtonCallback(nullptr, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    check("松开：IsMouseButtonReleased", IsMouseButtonReleased(MOUSE_BUTTON_LEFT));

    std::printf("== 滚轮 ==\n");
    scrollCallback(nullptr, 0.0, 1.0);
    check("滚轮 +1", GetMouseWheelMove() == 1.0f && GetMouseWheelMoveV().y == 1.0f);
    pollEvents();
    check("下一帧滚轮归零", GetMouseWheelMove() == 0.0f);

    std::printf("== 鼠标坐标映射（canvas 缩放/黑边就是靠它）==\n");
    cursorPosCallback(nullptr, 640.0, 360.0);
    check("原始坐标", GetMousePosition().x == 640.0f && GetMousePosition().y == 360.0f);
    // 模拟 canvas::update：窗口 1280x720 装 1280x720 设计分辨率 -> 偏移 0、缩放 1
    SetMouseOffset(0, 0);
    SetMouseScale(1.0f / 1.5f, 1.0f / 1.5f);
    check("缩放 1/1.5 后 x=426.67", GetMousePosition().x > 426.6f && GetMousePosition().x < 426.7f);
    // 模拟留黑边：偏移 -100，缩放 0.5
    SetMouseOffset(-100, -50);
    SetMouseScale(0.5f, 0.5f);
    check("偏移+缩放映射正确", GetMousePosition().x == 270.0f && GetMousePosition().y == 155.0f);
    check("GetMouseX 取整", GetMouseX() == 270);

    std::printf("== 退出键 ==\n");
    SetExitKey(KEY_NULL);
    g.shouldClose = false;
    keyCallback(nullptr, KEY_ESCAPE, 0, GLFW_PRESS, 0);
    check("关掉退出键后按 ESC 不退出", !g.shouldClose);
    SetExitKey(KEY_ESCAPE);
    keyCallback(nullptr, KEY_ESCAPE, 0, GLFW_PRESS, 0);
    check("退出键为 ESC 时按下即请求退出", g.shouldClose);

    std::printf("\n%s（%d 个失败）\n", failures == 0 ? "INPUT CHECK OK" : "INPUT CHECK FAILED",
                failures);
    return failures == 0 ? 0 : 1;
}
