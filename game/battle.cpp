#include "battle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "../core/canvas.h"
#include "../core/font.h"
#include "../core/lang.h"
#include "../renderer/renderer.h"

namespace
{
// ---------------- 角色数值表 ----------------
enum class SkillKind { BigHit, Splash, Heal, AtkUp, Stun };

struct CharStat
{
    int hp, atk, def, spd;
    const char* skill;
    SkillKind kind;
    int power;      // BigHit/Splash = 倍率百分比，Heal = 回复量，AtkUp = 加成百分比
};

const std::map<std::string, CharStat>& statTable()
{
    static const std::map<std::string, CharStat> t = {
        {"吴鸿韬", {92, 16, 6, 11, "精确观测", SkillKind::AtkUp, 30}},
        {"黎璘",   {86, 12, 7, 10, "云志",     SkillKind::Heal,   46}},
        {"梁知奕", {80, 18, 5, 13, "推演",     SkillKind::BigHit, 220}},
        {"尹博涛", {96, 17, 6, 14, "冲刺",     SkillKind::Splash, 110}},
        {"李君浩", {82, 13, 6,  9, "校准",     SkillKind::AtkUp,  25}},
        {"李俊辰", {88, 16, 6, 10, "打样",     SkillKind::Splash, 105}},
        {"邵清和", {102, 15, 8, 8, "熄灯",     SkillKind::Stun,   1}},
        {"顾星禾", {84, 15, 5, 12, "曝光",     SkillKind::BigHit, 200}},
        {"沈砚",   {78, 13, 5,  9, "背诵",     SkillKind::Heal,   40}},
        {"张誉腾", {94, 16, 7,  9, "签字",     SkillKind::Stun,   1}},
        {"魏思远", {98, 16, 7, 10, "观测台",   SkillKind::Heal,   52}},
    };
    return t;
}

CharStat statOf(const std::string& name)
{
    auto it = statTable().find(name);
    if (it != statTable().end()) return it->second;
    return CharStat{88, 14, 6, 10, "观测", SkillKind::BigHit, 180};
}

float frand(float a, float b)
{
    return a + (b - a) * (static_cast<float>(GetRandomValue(0, 1000)) / 1000.0f);
}

// ---- 敌人蓄力大招 ----
// 第 1、4、7、10 回合结束时预告，第 3、6、9、12 回合打出：玩家有整整一轮时间应对
// （守住被锁定的那位 / 治疗 / 用眩晕打断 / 抢在它出手前杀掉）。
constexpr int kChargeEvery = 4;
constexpr float kChargeMul = 1.8f;      // 大招伤害 = 攻击 × 该系数（单体）

// 敌人形象分类（名字里带什么就画成什么）
enum class EnemyLook { Light, Cloud, Noise, Solid };

EnemyLook lookOf(const std::string& name)
{
    if (name.find("光") != std::string::npos || name.find("灯") != std::string::npos ||
        name.find("霓虹") != std::string::npos)
        return EnemyLook::Light;
    if (name.find("云") != std::string::npos || name.find("雨") != std::string::npos ||
        name.find("雾") != std::string::npos)
        return EnemyLook::Cloud;
    if (name.find("杂讯") != std::string::npos || name.find("噪音") != std::string::npos ||
        name.find("流量") != std::string::npos)
        return EnemyLook::Noise;
    return EnemyLook::Solid;
}

Color lookColor(EnemyLook l)
{
    switch (l)
    {
    case EnemyLook::Light: return Color{255, 214, 120, 255};
    case EnemyLook::Cloud: return Color{150, 168, 200, 255};
    case EnemyLook::Noise: return Color{190, 110, 200, 255};
    default:               return Color{210, 120, 110, 255};
    }
}

void hpBar(Rectangle r, float ratio, Color fill, Color back)
{
    DrawRectangleRounded(r, r.height * 0.5f, 8, back);
    if (ratio > 0.001f)
        DrawRectangleRounded({r.x, r.y, r.width * std::clamp(ratio, 0.0f, 1.0f), r.height},
                             r.height * 0.5f, 8, fill);
}

const renderer::Palette& pal() { return renderer::palette(); }
Color withAlpha(Color c, unsigned char a) { c.a = a; return c; }
}

// ---------------------------------------------------------------- 初始化
void Battle::setup(const BattleDef& def)
{
    allies_.clear();
    enemies_.clear();
    log_.clear();
    popups_.clear();
    order_.clear();
    orderPos_ = 0;
    round_ = 0;
    phase_ = Phase::Intro;
    introT_ = 0.0f;
    resultT_ = 0.0f;
    timer_ = 0.0f;
    won_ = true;
    title_ = def.id;
    targetSel_ = 0;
    spHintT_ = 0.0f;

    for (const auto& name : def.party)
    {
        if (static_cast<int>(allies_.size()) >= kPartySize) break;
        CharStat s = statOf(name);
        Unit u;
        u.name = name;
        u.ally = true;
        u.hp = u.hpMax = s.hp;
        u.atk = s.atk;
        u.def = s.def;
        u.spd = s.spd;
        u.sp = 1;
        allies_.push_back(u);
    }
    if (allies_.empty())
    {
        CharStat s = statOf("吴鸿韬");
        Unit u;
        u.name = "吴鸿韬";
        u.ally = true;
        u.hp = u.hpMax = s.hp;
        u.atk = s.atk;
        u.def = s.def;
        u.spd = s.spd;
        u.sp = 1;
        allies_.push_back(u);
    }

    for (const auto& e : def.enemies)
    {
        if (static_cast<int>(enemies_.size()) >= kMaxEnemies) break;
        Unit u;
        u.name = e.name;
        u.ally = false;
        u.hp = u.hpMax = e.hp;
        u.atk = e.atk;
        u.def = std::max(0, e.atk / 3);
        u.spd = e.spd;
        u.image = e.image;
        enemies_.push_back(u);
    }
    if (enemies_.empty())
    {
        Unit u;
        u.name = "积云";
        u.ally = false;
        u.hp = u.hpMax = 120;
        u.atk = 12;
        u.def = 4;
        u.spd = 8;
        enemies_.push_back(u);
    }
    pushLog(lang::tr("battle.start"));
}

int Battle::mostHurtAlly() const
{
    int best = -1;
    float bestRatio = 2.0f;
    for (size_t i = 0; i < allies_.size(); ++i)
    {
        const Unit& u = allies_[i];
        if (!u.alive()) continue;
        const float ratio = u.hpMax > 0 ? static_cast<float>(u.hp) / u.hpMax : 0.0f;
        if (ratio < bestRatio) { bestRatio = ratio; best = static_cast<int>(i); }
    }
    return best;
}

int Battle::liveTargetFrom(int preferred) const
{
    const int n = static_cast<int>(enemies_.size());
    if (n == 0) return -1;
    const int start = (preferred >= 0 && preferred < n) ? preferred : 0;
    for (int k = 0; k < n; ++k)
    {
        const int i = (start + k) % n;
        if (enemies_[i].alive()) return i;
    }
    return -1;
}

void Battle::cycleTarget(int dir)
{
    const int n = static_cast<int>(enemies_.size());
    if (n == 0) return;
    int i = targetSel_;
    for (int k = 0; k < n; ++k)
    {
        i = (i + dir + n) % n;
        if (enemies_[i].alive()) { targetSel_ = i; return; }
    }
}

void Battle::armCharges()
{
    if (round_ % kChargeEvery != 1) return;
    const int fire = round_ + 2;
    const int mark = mostHurtAlly();
    // 只让一个敌人蓄力，别三个一起糊脸；按存活顺序轮着来
    std::vector<int> alive;
    for (size_t i = 0; i < enemies_.size(); ++i)
        if (enemies_[i].alive()) alive.push_back(static_cast<int>(i));
    if (alive.empty() || mark < 0) return;
    const int wave = round_ / kChargeEvery;          // 0,1,2,3…
    Unit& e = enemies_[alive[wave % static_cast<int>(alive.size())]];
    e.chargeRound = fire;
    e.chargeTarget = mark;
    pushLog(e.name + lang::tr("battle.charge_log") + allies_[mark].name);
}

// ---------------------------------------------------------------- 回合推进
void Battle::beginRound()
{
    ++round_;
    order_.clear();
    orderPos_ = 0;

    struct Ref { int idx; int spd; };
    std::vector<Ref> refs;
    for (size_t i = 0; i < allies_.size(); ++i)
        if (allies_[i].alive()) refs.push_back({static_cast<int>(i) + 1, allies_[i].spd});
    for (size_t i = 0; i < enemies_.size(); ++i)
        if (enemies_[i].alive()) refs.push_back({-(static_cast<int>(i) + 1), enemies_[i].spd});
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
        if (a.spd != b.spd) return a.spd > b.spd;
        return a.idx < b.idx;
    });
    for (const auto& r : refs) order_.push_back(r.idx);

    char buf[64];
    std::snprintf(buf, sizeof(buf), lang::tr("battle.round"), round_);
    pushLog(buf);

    if (round_ > maxRounds_)
    {
        won_ = false;
        phase_ = Phase::Result;
        resultT_ = 0.0f;
        pushLog(lang::tr("battle.timeout"));
    }
}

void Battle::nextTurn()
{
    if (phase_ == Phase::Result) return;
    while (orderPos_ < order_.size())
    {
        const int who = order_[orderPos_];
        if (who > 0)
        {
            Unit& u = allies_[who - 1];
            if (!u.alive()) { ++orderPos_; continue; }
            if (u.stun > 0) { --u.stun; ++orderPos_; continue; }
            phase_ = Phase::Command;
            timer_ = 0.0f;
            return;
        }
        Unit& u = enemies_[-who - 1];
        if (!u.alive()) { ++orderPos_; continue; }
        if (u.stun > 0)
        {
            --u.stun;
            if (u.chargeRound > 0)      // 眩晕打断蓄力
            {
                u.chargeRound = 0;
                u.chargeTarget = -1;
                pushLog(u.name + lang::tr("battle.charge_break"));
            }
            ++orderPos_;
            continue;
        }
        phase_ = Phase::Acting;
        timer_ = 0.5f;          // 敌人起手时间，玩家能看清是谁要动手
        return;
    }
    // 一轮打完：结算增益与 SP、清掉本回合的防御姿态，再预告下一轮的大招
    for (auto& u : allies_)
    {
        if (u.atkBuff > 0) --u.atkBuff;
        u.guard = 0;            // 守住只保护"当回合"，不是整场
    }
    for (auto& u : allies_) if (u.sp < u.spMax) ++u.sp;
    armCharges();
    beginRound();
    nextTurn();
}

void Battle::actAlly(int cmd)
{
    if (orderPos_ >= order_.size()) return;
    const int who = order_[orderPos_];
    if (who <= 0) return;
    Unit& me = allies_[who - 1];
    CharStat st = statOf(me.name);
    const int target = liveTargetFrom(targetSel_);
    if (target < 0) { checkEnd(); return; }
    targetSel_ = target;       // 选中的目标倒了就自动挪到下一个还活着的

    if (cmd == 2)                       // 守
    {
        me.guard = 1;
        if (me.sp < me.spMax) ++me.sp;
        pushLog(me.name + lang::tr("battle.log_guard"));
    }
    else if (cmd == 1 && me.sp >= kSkillCost)   // 技能
    {
        me.sp -= kSkillCost;
        char buf[96];
        std::snprintf(buf, sizeof(buf), lang::tr("battle.log_skill"), st.skill);
        pushLog(me.name + buf);
        switch (st.kind)
        {
        case SkillKind::BigHit:
            dealDamage(target,
                       std::max(1, static_cast<int>(me.atk * st.power / 100.0f * frand(0.95f, 1.05f))),
                       false);
            break;
        case SkillKind::Splash:
            for (size_t i = 0; i < enemies_.size(); ++i)
                if (enemies_[i].alive())
                    dealDamage(static_cast<int>(i),
                               std::max(1, static_cast<int>(me.atk * st.power / 100.0f * frand(0.9f, 1.1f))),
                               false);
            break;
        case SkillKind::Heal:
            healAll(st.power);
            break;
        case SkillKind::AtkUp:
            buffValue_ = st.power;
            for (auto& u : allies_) u.atkBuff = 2;
            pushLog(lang::tr("battle.log_buff"));
            break;
        case SkillKind::Stun:
            for (auto& u : enemies_) u.stun = std::max(u.stun, 1);
            pushLog(lang::tr("battle.log_stun"));
            break;
        }
    }
    else                                // 普攻
    {
        int dmg = std::max(1, static_cast<int>(me.atk * frand(0.9f, 1.15f) -
                                               enemies_[target].def * 0.4f));
        if (me.atkBuff > 0) dmg = static_cast<int>(dmg * (1.0f + buffValue_ / 100.0f));
        const bool crit = GetRandomValue(0, 99) < 12;
        if (crit) dmg = static_cast<int>(dmg * 1.6f);
        dealDamage(target, dmg, crit);
    }

    ++orderPos_;
    checkEnd();
}

void Battle::actEnemy(int idx)
{
    Unit& e = enemies_[idx];
    std::vector<int> alive;
    for (size_t i = 0; i < allies_.size(); ++i)
        if (allies_[i].alive()) alive.push_back(static_cast<int>(i));
    if (alive.empty()) { checkEnd(); return; }

    // 打在一位我方身上（守住减半），顺带出飘字和受击反馈
    auto hit = [&](int t, float raw, float popTime, float flashTime) {
        Unit& me2 = allies_[t];
        int dmg = std::max(1, static_cast<int>(raw - me2.def * 0.5f));
        if (me2.guard > 0) dmg = std::max(1, dmg / 2);
        me2.hp = std::max(0, me2.hp - dmg);
        me2.flash = flashTime;
        me2.shake = flashTime;
        Rectangle r{};
        allyRect(t, r);
        popups_.push_back({{r.x + r.width * 0.5f, r.y + 10.0f}, {dmg, popTime}});
    };

    // 蓄力大招：打在预告里标出的那一位身上（他若已经倒了就改成最残血的）
    if (e.chargeRound > 0 && round_ >= e.chargeRound)
    {
        int t = (e.chargeTarget >= 0 && e.chargeTarget < static_cast<int>(allies_.size()) &&
                 allies_[e.chargeTarget].alive()) ? e.chargeTarget : mostHurtAlly();
        e.chargeRound = 0;
        e.chargeTarget = -1;
        if (t >= 0)
        {
            hit(t, e.atk * kChargeMul * frand(0.95f, 1.05f), 1.2f, 0.5f);
            pushLog(e.name + lang::tr("battle.charge_fire") + allies_[t].name);
        }
        ++orderPos_;
        checkEnd();
        return;
    }

    const int t = alive[GetRandomValue(0, static_cast<int>(alive.size()) - 1)];
    hit(t, e.atk * frand(0.85f, 1.15f), 0.9f, 0.35f);
    pushLog(e.name + lang::tr("battle.log_enemy_hit") + allies_[t].name);
    ++orderPos_;
    checkEnd();
}

void Battle::dealDamage(int targetIdx, int amount, bool crit)
{
    if (targetIdx < 0 || targetIdx >= static_cast<int>(enemies_.size())) return;
    Unit& e = enemies_[targetIdx];
    if (!e.alive()) return;
    e.hp = std::max(0, e.hp - amount);
    e.flash = 0.35f;
    e.shake = 0.35f;
    Rectangle r{};
    enemyRect(targetIdx, r);
    popups_.push_back({{r.x + r.width * 0.5f, r.y + r.height * 0.4f},
                       {amount, crit ? 1.2f : 0.9f}});
    if (crit) pushLog(lang::tr("battle.log_crit"));
}

void Battle::healAll(int amount)
{
    for (size_t i = 0; i < allies_.size(); ++i)
    {
        Unit& u = allies_[i];
        if (!u.alive()) continue;
        const int before = u.hp;
        u.hp = std::min(u.hpMax, u.hp + amount);
        if (u.hp != before)
        {
            Rectangle r{};
            allyRect(static_cast<int>(i), r);
            popups_.push_back({{r.x + r.width * 0.5f, r.y + 10.0f}, {u.hp - before, 0.9f}});
        }
    }
    pushLog(lang::tr("battle.log_heal"));
}

void Battle::checkEnd()
{
    if (aliveEnemies() == 0)
    {
        won_ = true;
        phase_ = Phase::Result;
        resultT_ = 0.0f;
        pushLog(lang::tr("battle.win"));
    }
    else if (aliveAllies() == 0)
    {
        won_ = false;
        phase_ = Phase::Result;
        resultT_ = 0.0f;
        pushLog(lang::tr("battle.lose"));
    }
}

int Battle::aliveEnemies() const
{
    int n = 0;
    for (const auto& e : enemies_) if (e.alive()) ++n;
    return n;
}

int Battle::aliveAllies() const
{
    int n = 0;
    for (const auto& a : allies_) if (a.alive()) ++n;
    return n;
}

void Battle::pushLog(const std::string& s)
{
    log_.push_back(s);
    if (log_.size() > 4) log_.erase(log_.begin());
}

const char* Battle::skillName(int ally) const
{
    static std::string tmp;
    tmp = statOf(allies_[ally].name).skill;
    return tmp.c_str();
}

// ---------------------------------------------------------------- 每帧
void Battle::enemyRect(int idx, Rectangle& out) const
{
    const float w = static_cast<float>(canvas::width());
    const int n = static_cast<int>(enemies_.size());
    const float cw = 210.0f, ch = 150.0f, gap = 40.0f;
    const float total = n * cw + (n - 1) * gap;
    out = Rectangle{w * 0.5f - total * 0.5f + idx * (cw + gap), 104.0f, cw, ch};
}

void Battle::allyRect(int idx, Rectangle& out) const
{
    const float w = static_cast<float>(canvas::width());
    const int n = static_cast<int>(allies_.size());
    const float cw = 200.0f, ch = 210.0f, gap = 40.0f;
    const float total = n * cw + (n - 1) * gap;
    out = Rectangle{w * 0.5f - total * 0.5f - 60.0f + idx * (cw + gap), 430.0f, cw, ch};
}

bool Battle::isCommandHovered(int cmd, Rectangle& out) const
{
    const float w = static_cast<float>(canvas::width());
    const float h = static_cast<float>(canvas::height());
    const float bw = 178.0f, bh = 48.0f, gap = 12.0f;
    out = Rectangle{w - bw - 28.0f, h - 74.0f - (2 - cmd) * (bh + gap), bw, bh};
    return CheckCollisionPointRec(GetMousePosition(), out);
}

void Battle::update(float dt)
{
    time_ += dt;
    if (spHintT_ > 0.0f) spHintT_ = std::max(0.0f, spHintT_ - dt);
    for (auto& u : allies_) { u.flash = std::max(0.0f, u.flash - dt * 2.5f);
                              u.shake = std::max(0.0f, u.shake - dt * 2.5f); }
    for (auto& u : enemies_) { u.flash = std::max(0.0f, u.flash - dt * 2.5f);
                               u.shake = std::max(0.0f, u.shake - dt * 2.5f); }
    for (auto& p : popups_) p.second.second -= dt;
    popups_.erase(std::remove_if(popups_.begin(), popups_.end(),
                                 [](const auto& p) { return p.second.second <= 0.0f; }),
                  popups_.end());

    if (autoCmd_ >= 0 && phase_ == Phase::Command)
    {
        actAlly(autoCmd_);
        nextTurn();
        return;
    }

    switch (phase_)
    {
    case Phase::Intro:
        introT_ += dt;
        if (introT_ > 0.9f) { beginRound(); nextTurn(); }
        break;

    case Phase::Command:
    {
        // 三个指令按钮
        cmdHover_ = -1;
        for (int i = 0; i < 3; ++i)
        {
            Rectangle r{};
            if (isCommandHovered(i, r)) cmdHover_ = i;
            cmdHoverAnim_[i] = renderer::approach(cmdHoverAnim_[i], cmdHover_ == i ? 1.0f : 0.0f,
                                                  14.0f, dt);
        }
        const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
        const bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                           IsKeyPressed(KEY_KP_ENTER);

        // 选目标：点敌人卡片，或左右方向键切换
        targetSel_ = liveTargetFrom(targetSel_);
        if (IsKeyPressed(KEY_LEFT)) cycleTarget(-1);
        if (IsKeyPressed(KEY_RIGHT)) cycleTarget(1);
        if (click)
        {
            for (size_t i = 0; i < enemies_.size(); ++i)
            {
                if (!enemies_[i].alive()) continue;
                Rectangle er{};
                enemyRect(static_cast<int>(i), er);
                if (CheckCollisionPointRec(GetMousePosition(), er))
                {
                    targetSel_ = static_cast<int>(i);
                    break;
                }
            }
        }

        int cmd = -1;
        if (click && cmdHover_ >= 0) cmd = cmdHover_;
        else if (enter) cmd = 0;
        else if (IsKeyPressed(KEY_ONE)) cmd = 0;
        else if (IsKeyPressed(KEY_TWO)) cmd = 1;
        else if (IsKeyPressed(KEY_THREE)) cmd = 2;
        // SP 不够就只提示、不吃掉这一手，让玩家重新选
        if (cmd == 1 && allies_[std::abs(order_[orderPos_]) - 1].sp < kSkillCost)
        {
            spHintT_ = 1.4f;
            cmd = -1;
        }
        if (cmd >= 0)
        {
            actAlly(cmd);
            nextTurn();
        }
        break;
    }

    case Phase::Acting:
        timer_ -= dt;
        if (timer_ <= 0.0f)
        {
            const int who = orderPos_ < order_.size() ? order_[orderPos_] : 1;
            if (who < 0) actEnemy(-who - 1);
            else actAlly(0);
            nextTurn();
        }
        break;

    case Phase::Result:
        resultT_ += dt;
        break;
    }
}

void Battle::debugAuto(int frame)
{
    (void)frame;
    // 自检：轮到玩家就照一个固定策略出招，整场战斗不用人手
    if (phase_ != Phase::Command) return;
    Unit& me = allies_[std::abs(order_[orderPos_]) - 1];
    if (me.hp * 3 < me.hpMax && me.sp >= kSkillCost) autoCmd_ = 1;
    else if (me.sp >= kSkillCost) autoCmd_ = 1;
    else autoCmd_ = 0;
    actAlly(autoCmd_);
    nextTurn();
}

bool Battle::runHeadless(int maxSteps)
{
    const float dt = 1.0f / 60.0f;
    for (int step = 0; step < maxSteps && !finished(); ++step)
    {
        introT_ = (phase_ == Phase::Intro) ? introT_ + dt : introT_;
        if (phase_ == Phase::Intro && introT_ > 0.9f) { beginRound(); nextTurn(); continue; }
        if (phase_ == Phase::Command)
        {
            // 固定策略：有 SP 就放技能，否则普攻（走的是和玩家一样的 actAlly）
            Unit& me = allies_[std::abs(order_[orderPos_]) - 1];
            actAlly(me.sp >= kSkillCost ? 1 : 0);
            nextTurn();
            continue;
        }
        if (phase_ == Phase::Acting)
        {
            const int who = orderPos_ < order_.size() ? order_[orderPos_] : 1;
            if (who < 0) actEnemy(-who - 1);
            else actAlly(0);
            nextTurn();
            continue;
        }
        if (phase_ == Phase::Result) break;
    }
    if (std::getenv("QLWT_BATTLE_DEBUG"))
    {
        std::printf("[battle-dbg] %s round=%d won=%d | 我方", title_.c_str(), round_, won_ ? 1 : 0);
        for (const auto& a : allies_) std::printf(" %s=%d/%d", a.name.c_str(), a.hp, a.hpMax);
        std::printf(" | 敌方");
        for (const auto& e2 : enemies_) std::printf(" %s=%d/%d", e2.name.c_str(), e2.hp, e2.hpMax);
        std::printf("\n");
    }
    return finished();
}

// ---------------------------------------------------------------- 绘制
namespace
{
// 名字 → 立绘资源 id（战斗里画我方用）
const std::map<std::string, std::string>& idTable()
{
    static const std::map<std::string, std::string> t = {
        {"吴鸿韬", "wu_hongtao"}, {"黎璘", "li_lin"},     {"张誉腾", "zhang_yuteng"},
        {"梁知奕", "liang_zhiyi"}, {"尹博涛", "yin_botao"}, {"李君浩", "li_junhao"},
        {"李俊辰", "li_junchen"}, {"邵清和", "shao_qinghe"}, {"魏思远", "wei_siyuan"},
        {"顾星禾", "gu_xinghe"},  {"沈砚", "shen_yan"},
    };
    return t;
}

Texture2D bodyTexture(const std::string& name)
{
    static std::map<std::string, Texture2D> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    Texture2D tex{};
    auto idIt = idTable().find(name);
    if (idIt != idTable().end())
    {
        std::string path = "assets/char/" + idIt->second + "_body.png";
        if (FileExists(path.c_str()))
        {
            tex = LoadTexture(path.c_str());
            if (tex.id)
            {
                GenTextureMipmaps(&tex);
                SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
            }
        }
    }
    cache[name] = tex;
    return tex;
}

// 敌人贴图：优先用剧本里指定的那张，否则按形象类别找 assets/enemy/<类别>.png。
// 都没有就返回空贴图，绘制时退回程序化图形（发光球 / 云团 / 噪点 / 六边形）。
// 类别目录用 ASCII 文件名，避免中文路径在 Windows 下读不出来的老问题。
const char* categoryFile(EnemyLook look)
{
    switch (look)
    {
    case EnemyLook::Light: return "light";
    case EnemyLook::Cloud: return "cloud";
    case EnemyLook::Noise: return "noise";
    default:               return "solid";
    }
}

struct EnemyTex
{
    Texture2D tex{};
    bool floating = false;      // 贴图自带透明通道 → 当浮动精灵画
};

const EnemyTex& enemyTexture(const std::string& explicitPath, const std::string& name)
{
    static std::map<std::string, EnemyTex> cache;
    std::string path = explicitPath;
    if (path.empty()) path = std::string("assets/enemy/") + categoryFile(lookOf(name)) + ".png";
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    EnemyTex rec;
    if (FileExists(path.c_str()))
    {
        Image img = LoadImage(path.c_str());
        if (img.data)
        {
            // 采样看是不是"真有透明底"：AI 出的立绘卡是不透明的，就按卡片画；
            // 手绘 / 抠好的透明 PNG 则当浮动精灵。
            if (img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 ||
                img.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA)
            {
                Color* px = LoadImageColors(img);
                const int total = img.width * img.height;
                int seen = 0, clear = 0;
                for (int i = 0; i < total; i += 7)
                {
                    ++seen;
                    if (px[i].a < 200) ++clear;
                }
                UnloadImageColors(px);
                rec.floating = seen > 0 && clear > seen / 8;   // >12.5% 透明 = 浮动
            }
            rec.tex = LoadTextureFromImage(img);
            UnloadImage(img);
            if (rec.tex.id)
            {
                GenTextureMipmaps(&rec.tex);
                SetTextureFilter(rec.tex, TEXTURE_FILTER_TRILINEAR);
                std::printf("[battle] 敌人贴图：%s（%s）\n", path.c_str(),
                            rec.floating ? "浮动精灵" : "立绘卡");
            }
        }
    }
    cache[path] = rec;
    return cache[path];
}

// 敌人形象：按名字分类画成不同形状
void drawEnemyShape(Rectangle r, const std::string& name, float flash, float t)
{
    const EnemyLook look = lookOf(name);
    const Color c = lookColor(look);
    const float cx = r.x + r.width * 0.5f;
    const float cy = r.y + r.height * 0.5f;
    const float pulse = 1.0f + 0.03f * std::sin(t * 2.2f);
    Color fill = flash > 0.0f ? Color{255, 255, 255, static_cast<unsigned char>(200 * flash)} : c;
    fill.a = 220;

    switch (look)
    {
    case EnemyLook::Light:      // 发光球：几层同心圆
        for (int i = 4; i >= 1; --i)
        {
            Color g = c;
            g.a = static_cast<unsigned char>(28 * i + 20);
            DrawCircleV({cx, cy}, r.height * 0.42f * pulse * (i * 0.32f + 0.3f), g);
        }
        DrawCircleV({cx, cy}, r.height * 0.26f * pulse, fill);
        break;
    case EnemyLook::Cloud:      // 云团：几个圆叠起来
    {
        const float rr = r.height * 0.26f;
        DrawCircleV({cx - rr * 0.9f, cy + rr * 0.25f}, rr * 0.95f, fill);
        DrawCircleV({cx + rr * 0.85f, cy + rr * 0.2f}, rr * 0.85f, fill);
        DrawCircleV({cx, cy - rr * 0.25f}, rr * 1.15f, fill);
        DrawRectangleRounded({cx - rr * 1.7f, cy + rr * 0.1f, rr * 3.4f, rr * 1.0f},
                             rr * 0.5f, 10, fill);
        break;
    }
    case EnemyLook::Noise:      // 雪花噪点
    {
        const int n = 90;
        for (int i = 0; i < n; ++i)
        {
            const float fx = cx + std::sin(i * 12.9898f) * r.width * 0.34f;
            const float fy = cy + std::cos(i * 78.233f) * r.height * 0.34f;
            Color g = c;
            g.a = static_cast<unsigned char>(90 + (i * 37) % 140);
            DrawRectangle(static_cast<int>(fx), static_cast<int>(fy), 3, 3, g);
        }
        break;
    }
    default:                    // 六边形
    {
        Vector2 pts[6];
        for (int i = 0; i < 6; ++i)
        {
            const float a = (i * 60.0f + t * 12.0f) * DEG2RAD;
            pts[i] = {cx + std::cos(a) * r.height * 0.4f * pulse,
                      cy + std::sin(a) * r.height * 0.4f * pulse};
        }
        DrawTriangleFan(pts, 6, fill);
        break;
    }
    }
}
}

void Battle::draw(const FontManager& fonts, float renderScale)
{
    const float rs = renderScale > 0.0f ? renderScale : 1.0f;
    const float w = static_cast<float>(canvas::width());
    const float h = static_cast<float>(canvas::height());
    const Color acc = renderer::accent();

    // 底：暗色渐变，把后面的剧情画面压下去
    DrawRectangleGradientV(0, 0, static_cast<int>(w), static_cast<int>(h),
                           Color{8, 10, 20, 230}, Color{14, 18, 34, 242});

    // ---- 标题条 ----
    fonts.draw(lang::tr("battle.title"), 30.0f, 22.0f, 26.0f, withAlpha(pal().text, 235), 2.4f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), lang::tr("battle.round_info"), round_, maxRounds_);
    fonts.draw(buf, 30.0f, 58.0f, 18.0f, withAlpha(pal().textDim, 190), 1.8f);
    {
        std::string t = lang::tr("battle.window") + std::string("：") + title_;
        Vector2 m = fonts.measure(t, 18.0f);
        fonts.draw(t, w - m.x - 30.0f, 30.0f, 18.0f, withAlpha(pal().textDim, 190), 1.8f);
    }

    // ---- 敌人 ----
    for (size_t i = 0; i < enemies_.size(); ++i)
    {
        const Unit& e = enemies_[i];
        Rectangle r{};
        enemyRect(static_cast<int>(i), r);
        const float sx = e.shake > 0.0f ? std::sin(time_ * 60.0f) * e.shake * 10.0f : 0.0f;
        r.x += sx;
        if (!e.alive())
        {
            DrawRectangleRounded(r, 12.0f, 10, Color{20, 24, 40, 120});
            Vector2 m = fonts.measure(lang::tr("battle.enemy_down"), 20.0f);
            fonts.draw(lang::tr("battle.enemy_down"), r.x + (r.width - m.x) * 0.5f,
                       r.y + r.height * 0.45f, 20.0f, withAlpha(pal().text, 130), 2.0f);
        }
        else
        {
            // 脚下一层淡影，别让它飘着
            DrawEllipse(static_cast<int>(r.x + r.width * 0.5f),
                        static_cast<int>(r.y + r.height - 26.0f),
                        r.width * 0.28f, 8.0f, Color{0, 0, 0, 70});

            const EnemyTex& et = enemyTexture(e.image, e.name);
            if (et.tex.id && et.floating)
            {
                // 等比塞进卡片，底部给名字 / 血条留位置
                const float maxW = r.width - 26.0f;
                const float maxH = r.height - 32.0f;
                const float sc = std::min(maxW / static_cast<float>(et.tex.width),
                                          maxH / static_cast<float>(et.tex.height));
                const float dw = static_cast<float>(et.tex.width) * sc;
                const float dh = static_cast<float>(et.tex.height) * sc;
                Rectangle dst{r.x + (r.width - dw) * 0.5f,
                              r.y + (r.height - 28.0f - dh) * 0.5f + 2.0f, dw, dh};
                DrawTexturePro(et.tex,
                               {0, 0, static_cast<float>(et.tex.width),
                                static_cast<float>(et.tex.height)},
                               dst, {0, 0}, 0.0f, WHITE);
                if (e.flash > 0.0f)     // 受击闪白（贴图是彩色的，只能叠一层白）
                {
                    BeginScissorMode(static_cast<int>(r.x * rs), static_cast<int>(r.y * rs),
                                     static_cast<int>(r.width * rs),
                                     static_cast<int>(r.height * rs));
                    DrawRectangle(static_cast<int>(dst.x), static_cast<int>(dst.y),
                                  static_cast<int>(dst.width), static_cast<int>(dst.height),
                                  Color{255, 255, 255,
                                        static_cast<unsigned char>(190.0f * e.flash)});
                    EndScissorMode();
                }
            }
            else if (et.tex.id)
            {
                // 立绘卡：圆角底板 + 内嵌图（cover 裁切，填满卡片）
                DrawRectangleRounded(r, 12.0f, 10, Color{16, 20, 36, 235});
                Rectangle inner{r.x + 6.0f, r.y + 6.0f, r.width - 12.0f, r.height - 12.0f};
                const float sc = std::max(inner.width / static_cast<float>(et.tex.width),
                                          inner.height / static_cast<float>(et.tex.height));
                const float dw = static_cast<float>(et.tex.width) * sc;
                const float dh = static_cast<float>(et.tex.height) * sc;
                BeginScissorMode(static_cast<int>(inner.x * rs), static_cast<int>(inner.y * rs),
                                 static_cast<int>(inner.width * rs),
                                 static_cast<int>(inner.height * rs));
                Rectangle dst{inner.x + (inner.width - dw) * 0.5f,
                              inner.y + (inner.height - dh) * 0.5f, dw, dh};
                DrawTexturePro(et.tex,
                               {0, 0, static_cast<float>(et.tex.width),
                                static_cast<float>(et.tex.height)},
                               dst, {0, 0}, 0.0f, WHITE);
                if (e.flash > 0.0f)
                    DrawRectangle(static_cast<int>(inner.x), static_cast<int>(inner.y),
                                  static_cast<int>(inner.width), static_cast<int>(inner.height),
                                  Color{255, 255, 255,
                                        static_cast<unsigned char>(190.0f * e.flash)});
                EndScissorMode();
                // 顶部压一层暗渐变，让名字 / 血条和标记读得清
                DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y),
                                       static_cast<int>(r.width), static_cast<int>(r.height),
                                       Color{0, 0, 0, 90}, Color{0, 0, 0, 30});
            }
            else
            {
                drawEnemyShape(r, e.name, e.flash, time_);
            }
        }

        // 目标高亮：当前选中的敌人（攻击 / 单体技能会打它）
        if (e.alive() && phase_ == Phase::Command && static_cast<int>(i) == targetSel_)
        {
            const float pulse = 0.6f + 0.4f * std::sin(time_ * 5.0f);
            renderer::drawRoundedBorder(
                r, 12.0f, 3.0f,
                withAlpha(pal().highlight, static_cast<unsigned char>(255 * pulse)));
            const std::string tag = lang::tr("battle.target");
            Vector2 tm = fonts.measure(tag, 17.0f);
            fonts.draw(tag, r.x + (r.width - tm.x) * 0.5f, r.y - 22.0f, 17.0f,
                       withAlpha(pal().highlight, 235), 1.7f);
        }

        // 蓄力预告：名字上面挂个会呼吸的红色标记，玩家有一整轮时间应对
        if (e.alive() && e.chargeRound > 0)
        {
            const float pulse = 0.55f + 0.45f * std::sin(time_ * 7.0f);
            const std::string tag = lang::tr("battle.charge");
            Vector2 tm = fonts.measure(tag, 17.0f);
            Rectangle badge{r.x + (r.width - tm.x) * 0.5f - 12.0f, r.y + 6.0f,
                            tm.x + 24.0f, 27.0f};
            renderer::drawPanel(badge,
                                withAlpha(Color{140, 44, 44, 255},
                                          static_cast<unsigned char>(195 + 50 * pulse)),
                                8.0f, withAlpha(Color{255, 150, 130, 255}, 235), 2.0f);
            fonts.draw(tag, badge.x + 12.0f, badge.y + 3.0f, 17.0f,
                       Color{255, 232, 228, 255}, 1.7f);
        }

        Vector2 nm = fonts.measure(e.name, 19.0f);
        fonts.draw(e.name, r.x + (r.width - nm.x) * 0.5f, r.y + r.height + 4.0f, 19.0f,
                   withAlpha(pal().text, 225), 1.8f);
        hpBar({r.x + 10.0f, r.y + r.height + 30.0f, r.width - 20.0f, 9.0f},
              e.hpMax > 0 ? static_cast<float>(e.hp) / e.hpMax : 0.0f,
              e.alive() ? lookColor(lookOf(e.name)) : Color{90, 96, 110, 200},
              Color{30, 34, 52, 220});
    }

    // ---- 战斗记录 ----
    for (size_t i = 0; i < log_.size(); ++i)
    {
        const float fade = 1.0f - 0.12f * static_cast<float>(log_.size() - 1 - i);
        fonts.draw(log_[i], 30.0f, 300.0f + static_cast<float>(i) * 26.0f, 18.0f,
                   withAlpha(pal().text, static_cast<unsigned char>(210 * fade)), 1.8f);
    }

    // ---- 我方 ----
    for (size_t i = 0; i < allies_.size(); ++i)
    {
        const Unit& u = allies_[i];
        Rectangle r{};
        allyRect(static_cast<int>(i), r);
        const float sx = u.shake > 0.0f ? std::sin(time_ * 60.0f) * u.shake * 8.0f : 0.0f;
        const bool acting = (phase_ == Phase::Command && orderPos_ < order_.size() &&
                             order_[orderPos_] == static_cast<int>(i) + 1);
        const float lift = acting ? -6.0f : 0.0f;
        Rectangle card{r.x + sx, r.y + lift, r.width, r.height};

        DrawRectangleRounded(card, 14.0f, 12,
                             u.alive() ? Color{26, 32, 54, 210} : Color{20, 22, 34, 190});
        if (acting)
            renderer::drawRoundedBorder(card, 14.0f, 2.5f, withAlpha(pal().highlight, 230));

        Texture2D tex = bodyTexture(u.name);
        if (tex.id)
        {
            const float th = card.height - 66.0f;
            const float tw = tex.width * (th / tex.height);
            Rectangle dst{card.x + (card.width - tw) * 0.5f, card.y + card.height - th - 62.0f,
                          tw, th};
            BeginScissorMode(static_cast<int>(card.x * rs), static_cast<int>(card.y * rs),
                             static_cast<int>(card.width * rs),
                             static_cast<int>(card.height * rs));
            DrawTexturePro(tex,
                           {0, 0, static_cast<float>(tex.width), static_cast<float>(tex.height)},
                           dst, {0, 0}, 0.0f,
                           u.alive() ? WHITE
                                     : Color{120, 130, 150, 190});
            EndScissorMode();
        }
        else
        {
            Vector2 m = fonts.measure(u.name, 34.0f);
            fonts.draw(u.name, card.x + (card.width - m.x) * 0.5f, card.y + 60.0f, 34.0f,
                       withAlpha(pal().text, 200), 2.6f);
        }

        Vector2 nm = fonts.measure(u.name, 18.0f);
        fonts.draw(u.name, card.x + (card.width - nm.x) * 0.5f, card.y + card.height - 56.0f,
                   18.0f, withAlpha(pal().text, 230), 1.8f);
        hpBar({card.x + 12.0f, card.y + card.height - 30.0f, card.width - 24.0f, 8.0f},
              u.hpMax > 0 ? static_cast<float>(u.hp) / u.hpMax : 0.0f,
              Color{120, 220, 150, 235}, Color{30, 34, 52, 220});
        // SP：技能点
        for (int s = 0; s < u.spMax; ++s)
        {
            Rectangle p{card.x + 12.0f + s * 15.0f, card.y + card.height - 17.0f, 11.0f, 11.0f};
            DrawRectangleRounded(p, 3.0f, 6,
                                 s < u.sp ? withAlpha(acc, 235) : Color{40, 46, 66, 220});
        }
        if (!u.alive())
            fonts.draw(lang::tr("battle.ally_down"),
                       card.x + (card.width - fonts.measure(lang::tr("battle.ally_down"), 17.0f).x) * 0.5f,
                       card.y + card.height - 78.0f, 17.0f, Color{255, 150, 150, 220}, 1.7f);

        // 被敌方大招锁定的那位：红框 + 标记，提醒玩家守住 / 治疗 / 打断
        bool targeted = false;
        for (const auto& e : enemies_)
            if (e.alive() && e.chargeRound > 0 && e.chargeTarget == static_cast<int>(i))
                targeted = true;
        if (targeted && u.alive())
        {
            const float pulse = 0.55f + 0.45f * std::sin(time_ * 7.0f);
            renderer::drawRoundedBorder(
                card, 14.0f, 3.0f,
                withAlpha(Color{255, 120, 110, 255},
                          static_cast<unsigned char>(200 + 55 * pulse)));
            const std::string tag = lang::tr("battle.targeted");
            Vector2 tm = fonts.measure(tag, 17.0f);
            Rectangle badge{card.x + (card.width - tm.x) * 0.5f - 12.0f, card.y + 6.0f,
                            tm.x + 24.0f, 27.0f};
            renderer::drawPanel(badge,
                                withAlpha(Color{170, 40, 40, 255},
                                          static_cast<unsigned char>(195 + 50 * pulse)),
                                8.0f, withAlpha(Color{255, 150, 130, 255}, 235), 2.0f);
            fonts.draw(tag, badge.x + 12.0f, badge.y + 3.0f, 17.0f,
                       Color{255, 232, 228, 255}, 1.7f);
        }
    }

    // ---- 指令菜单 ----
    if (phase_ == Phase::Command)
    {
        const char* keys[3] = {"battle.cmd_attack", "battle.cmd_skill", "battle.cmd_guard"};
        for (int i = 0; i < 3; ++i)
        {
            Rectangle r{};
            isCommandHovered(i, r);
            Unit& me = allies_[std::abs(order_[orderPos_]) - 1];
            const bool canSkill = (i != 1) || me.sp >= kSkillCost;
            std::string label = lang::tr(keys[i]);
            if (i == 1)
                label += std::string("（") + skillName(static_cast<int>(std::abs(order_[orderPos_]) - 1)) +
                         "）";
            renderer::drawButton(r, label,
                                 fonts.font(), 20.0f,
                                 cmdHoverAnim_[i],
                                 cmdHover_ == i, IsMouseButtonDown(MOUSE_BUTTON_LEFT) && cmdHover_ == i,
                                 canSkill, Color{0, 0, 0, 0});
        }
        std::string who = allies_[std::abs(order_[orderPos_]) - 1].name + lang::tr("battle.turn_hint");
        fonts.draw(who, w - 470.0f, h - 330.0f, 20.0f, withAlpha(pal().text, 220), 2.0f);
        if (spHintT_ > 0.0f)
        {
            const float a = std::clamp(spHintT_ / 0.4f, 0.0f, 1.0f);
            fonts.draw(lang::tr("battle.sp_lack"), w - 470.0f, h - 300.0f, 18.0f,
                       withAlpha(Color{255, 150, 140, 255},
                                 static_cast<unsigned char>(235 * a)), 1.8f);
        }
    }

    // ---- 飘字 ----
    for (const auto& p : popups_)
    {
        const float t = p.second.second;
        const float a = std::clamp(t * 1.6f, 0.0f, 1.0f);
        char num[24];
        std::snprintf(num, sizeof(num), "%d", p.second.first);
        Vector2 m = fonts.measure(num, 30.0f);
        fonts.draw(num, p.first.x - m.x * 0.5f, p.first.y - (1.0f - t) * 34.0f, 30.0f,
                   withAlpha(Color{255, 232, 150, 255}, static_cast<unsigned char>(235 * a)),
                   2.4f, true);
    }

    // ---- 结果 ----
    if (phase_ == Phase::Result)
    {
        const float a = std::clamp(resultT_ * 3.0f, 0.0f, 1.0f);
        DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h),
                      Color{0, 0, 0, static_cast<unsigned char>(150 * a)});
        const char* key = won_ ? "battle.win" : "battle.lose";
        Vector2 m = fonts.measure(lang::tr(key), 54.0f);
        fonts.draw(lang::tr(key), w * 0.5f - m.x * 0.5f, h * 0.42f, 54.0f,
                   withAlpha(won_ ? Color{180, 240, 190, 255} : Color{235, 170, 170, 255},
                             static_cast<unsigned char>(255 * a)), 3.0f, true);
        Vector2 m2 = fonts.measure(lang::tr("battle.result_hint"), 19.0f);
        fonts.draw(lang::tr("battle.result_hint"), w * 0.5f - m2.x * 0.5f, h * 0.42f + 74.0f,
                   19.0f, withAlpha(pal().text, static_cast<unsigned char>(190 * a)), 1.9f);
    }
}
