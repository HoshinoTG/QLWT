#pragma once

class Engine;

// 场景基类：标题 / 设置 / 游戏 共用同一套更新-绘制循环
class Scene
{
public:
    explicit Scene(Engine* e) : engine_(e) {}
    virtual ~Scene() = default;

    virtual void update(float dt) = 0;
    virtual void draw() = 0;

    // 毛玻璃/液态玻璃的折射源由渲染器实时抓取当前画面（renderer.cpp 的
    // captureGlassSource），场景不再需要额外声明"背景快照"。

    // 自检模式钩子：帧号驱动自动操作
    virtual void debugAuto(int /*frame*/) {}
    // 自检模式钩子：整帧绘制完成（EndDrawing 之后）调用
    virtual void afterDraw() {}

protected:
    Engine* engine() const { return engine_; }

    // 场景的入场淡入不再由各场景自己画：统一交给引擎的转场
    // （Engine::switchScene 会播一段"从黑场/擦除进来"的入场，见 gfx/post.cpp）
    // 场景想指定转场方式，就在 switchScene 之前调 engine()->setTransition(...)

private:
    Engine* engine_ = nullptr;
};
