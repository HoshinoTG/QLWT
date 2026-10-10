#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gfx/gfx.h"

#include "../renderer/renderer.h"

#include "../core/script.h"
#include "scene.h"

// 角色图集：剧情里登场过的角色自动收录，没登场的显示成剪影。
// 开始菜单进入；点角色看大图，Esc / 返回回标题。
class GalleryScene : public Scene
{
public:
    GalleryScene(Engine* e, std::string scriptPath, std::shared_ptr<Script> script,
                 std::string parseError);

    void update(float dt) override;
    void draw() override;
    void debugAuto(int frame) override;

private:
    struct Entry
    {
        const char* name;
        const char* id;
        const char* role;
        const char* tagline;
        Color color;
    };

    Rectangle cardRect(int idx) const;
    static const std::vector<Entry>& roster();
    void openDetail(int idx);
    void backToTitle();

    std::string scriptPath_;
    std::string parseError_;
    std::shared_ptr<Script> script_;

    std::vector<char> seen_;        // 构造时读一次 progress.dat
    std::vector<Texture2D> body_;   // 懒加载立绘
    int seenCount_ = 0;

    int hover_ = -1;
    int sel_ = 0;
    float hoverAnim_[12] = {};
    int detail_ = -1;
    float detailT_ = 0.0f;
    renderer::Spring detailSpring_{0.0f, 0.0f, 520.0f, 34.0f};   // 详情页弹入（更脆一点）
    int dbgFrame_ = 0;             // 自检用：进场后自己数帧（不依赖全局帧号）
};
