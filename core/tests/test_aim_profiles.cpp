// test_aim_profiles.cpp — 瞄准档位（多热键）数据层验收。
//
// 背景：面板「热键与类别」页每张卡片 = 一个档位。core 侧原来只有一个平铺热键组
// （aim_hotkey / aim_hotkey2 / aim_hotkey_mode），面板却能加任意多张卡 ⇒ 第 2 张起
// 保存时静默丢弃。2026-09-24 把真源换成 MouseProfile.aim_profiles（矢量），
// 老平铺 JSON key 降级为「数组缺失时的第 0 档合成源」+「序列化时的只写镜像」。
//
// 本文件钉住四件事，任何一条被改坏都会让多档位静默退化成"只有一个键能用"：
//   1) 命中判定（any / all）与「任意两档互斥 ⇒ 最多命中一档」这条不变量；
//   2) 输出闸门用的键位并集必须覆盖每一档的每一个键位；
//   3) 多档序列化往返逐字段不丢（含 class_offsets / class_filter / fov_scale）；
//   4) 老配置（只有平铺 key）必须合成出与升级前逐位等价的单档。
//
// 断言一律走 check()（本文件自带 main），不用裸 assert —— Release/NDEBUG 下裸 assert
// 会被整段编译掉，测试会退化成恒过。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "model/RuntimeProfile.hpp"
#include "mouse/MouseTypes.hpp"

using ttbox::core::JsonValue;
using ttbox::core::RuntimeProfile;
using ttbox::core::aim::AimHotkeyProfile;
using ttbox::core::aim::ClassOffset;

namespace {

int g_fails = 0;

void check(bool ok, const std::string& name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_fails;
}

void check_eq_i64(int64_t got, int64_t want, const std::string& name) {
    if (got != want) {
        std::printf("[FAIL] %s（got=%lld want=%lld）\n", name.c_str(),
                    static_cast<long long>(got), static_cast<long long>(want));
        ++g_fails;
    } else {
        std::printf("[PASS] %s\n", name.c_str());
    }
}

void check_eq_f(float got, float want, const std::string& name) {
    const float d = got - want;
    if (d > 1e-6f || d < -1e-6f) {
        std::printf("[FAIL] %s（got=%f want=%f）\n", name.c_str(),
                    static_cast<double>(got), static_cast<double>(want));
        ++g_fails;
    } else {
        std::printf("[PASS] %s\n", name.c_str());
    }
}

AimHotkeyProfile mk(uint8_t hk, uint8_t hk2, int mode) {
    AimHotkeyProfile ap;
    ap.hotkey = hk;
    ap.hotkey2 = hk2;
    ap.hotkey_mode = mode;
    return ap;
}

RuntimeProfile from_text(const std::string& text, const std::string& tag) {
    auto res = ttbox::core::json_parse(text);
    if (!res.ok) {
        std::printf("[FAIL] %s JSON 解析失败\n", tag.c_str());
        ++g_fails;
        return RuntimeProfile{};
    }
    return RuntimeProfile::from_json(res.value);
}

// 鼠标五键位图（左1 右2 中4 侧1 8 侧2 16）。
constexpr uint16_t kAllButtons = 0x1F;

}  // namespace

int main() {
    // ================= 1. 默认值 / 非空不变量 =================
    {
        RuntimeProfile p;
        check_eq_i64(static_cast<int64_t>(p.mouse.aim_profiles.size()), 1,
                     "默认 MouseProfile 自带一档（数组非空是不变量）");
        check_eq_i64(p.mouse.aim_profiles[0].hotkey, 0x02, "默认档主热键 = 右键 0x02");
        check_eq_i64(p.mouse.aim_profiles[0].hotkey2, 0x00, "默认档副热键 = 不使用");
        check_eq_i64(p.mouse.aim_profiles[0].hotkey_mode, 0, "默认档触发方式 = any");
        check_eq_f(p.mouse.aim_profiles[0].sensitivity, 1.0f, "默认档移动倍率 = 1.0");
        check_eq_f(p.mouse.aim_profiles[0].fov_scale, 1.0f, "默认档 FOV 倍率 = 1.0");
        // V3 阶段 2：默认 1.0 = 腰射 = 误差不做倍率折算 ⇒ 老配置行为不变。
        check_eq_f(p.mouse.aim_profiles[0].zoom_scale, 1.0f, "默认档倍镜倍率 = 1.0（腰射）");
    }

    // ================= 2. 命中判定 =================
    {
        const auto any_p = mk(0x02, 0x00, 0);   // 右键，any
        check(ttbox::core::aim::aim_hotkey_profile_hit(any_p, 0x02), "any+按主键 -> 命中");
        check(!ttbox::core::aim::aim_hotkey_profile_hit(any_p, 0x01), "any+按无关键 -> 不命中");
        check(!ttbox::core::aim::aim_hotkey_profile_hit(any_p, 0x00), "any+不按键 -> 不命中");

        const auto any2 = mk(0x02, 0x08, 0);    // 主右键 / 副侧1，any
        check(ttbox::core::aim::aim_hotkey_profile_hit(any2, 0x02), "any双键+只按主键 -> 命中");
        check(ttbox::core::aim::aim_hotkey_profile_hit(any2, 0x08), "any双键+只按副键 -> 命中");
        check(ttbox::core::aim::aim_hotkey_profile_hit(any2, 0x0A), "any双键+都按 -> 命中");

        const auto all2 = mk(0x02, 0x08, 1);    // all：必须同时按下
        check(ttbox::core::aim::aim_hotkey_profile_hit(all2, 0x0A), "all+两键同按 -> 命中");
        check(!ttbox::core::aim::aim_hotkey_profile_hit(all2, 0x02), "all+只按主键 -> 不命中");
        check(!ttbox::core::aim::aim_hotkey_profile_hit(all2, 0x08), "all+只按副键 -> 不命中");

        // 副键未配（0）时 all 模式永不命中 —— 面板已禁止这种配置，这里 fail-closed。
        const auto all_nofu = mk(0x02, 0x00, 1);
        check(!ttbox::core::aim::aim_hotkey_profile_hit(all_nofu, 0x02),
              "all+副键未配 -> 永不命中（fail-closed，不静默退化成 any）");

        // 主键为 0 但副键有效 ⇒ any 模式仍可只靠副键触发（与改档位之前的实现逐位一致）。
        const auto side_only = mk(0x00, 0x08, 0);
        check(ttbox::core::aim::aim_hotkey_profile_hit(side_only, 0x08),
              "any+主键为 0 但副键有效 -> 仅副键命中");
        check(!ttbox::core::aim::aim_hotkey_profile_hit(side_only, 0x02),
              "any+主键为 0 -> 主键位不再触发");

        // 主副键都为 0 = 配置缺失 ⇒ 任何按键都不命中（fail-closed，不静默退化成"随便按都瞄"）。
        const auto no_keys = mk(0x00, 0x00, 0);
        check(!ttbox::core::aim::aim_hotkey_profile_hit(no_keys, kAllButtons),
              "any+主副键都为 0 -> 永不命中（配置缺失 fail-closed）");
    }

    // ================= 3. 选档 =================
    {
        ttbox::core::aim::MouseProfile mp;
        mp.aim_profiles = {mk(0x02, 0x00, 0), mk(0x01, 0x00, 0), mk(0x04, 0x08, 1)};
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, 0x02), 0, "按右键 -> 档 0");
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, 0x01), 1, "按左键 -> 档 1");
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, 0x0C), 2, "中+侧1 同按 -> 档 2(all)");
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, 0x04), -1,
                     "只按中键 -> 档 2 是 all 模式，不命中 -> -1");
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, 0x10), -1, "按无关键 -> -1");
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, 0x00), -1, "不按键 -> -1");
    }

    // ================= 4. 「键位互斥」能保证什么、不能保证什么 =================
    // 这里刻意把"互斥 ⇒ 选档唯一"这个**错误**推论的反例钉住。
    // 面板保存时那条"禁止重叠"校验挡的是"同一个键给两档"，
    // 它挡不住"同时按下两档各自的键"（左键开火 + 右键瞄准同按）——
    // 位图能同时置多位，物理上禁不掉。所以选档唯一性其实来自
    // 「命中即返回」的顺序优先，而不是互斥。搞混这一点会让以后的人以为可以随便加档。
    {
        ttbox::core::aim::MouseProfile mp;
        mp.aim_profiles = {mk(0x02, 0x00, 0), mk(0x01, 0x00, 0), mk(0x04, 0x08, 1),
                           mk(0x10, 0x00, 0)};

        // (a) 单个键按下：互斥 ⇒ 最多命中一档。这是面板那条校验真正提供的东西。
        int worst_single = 0;
        for (uint16_t b = 1; b <= kAllButtons; b = static_cast<uint16_t>(b << 1)) {
            int hits = 0;
            for (const auto& ap : mp.aim_profiles) {
                if (ttbox::core::aim::aim_hotkey_profile_hit(ap, b)) ++hits;
            }
            if (hits > worst_single) worst_single = hits;
        }
        check_eq_i64(worst_single, 1, "键位互斥的档表：任何【单个】键按下最多命中一档");

        // (b) 多键同按：互斥挡不住 —— 五键同按时四档全部命中。
        int hits_all = 0;
        for (const auto& ap : mp.aim_profiles) {
            if (ttbox::core::aim::aim_hotkey_profile_hit(ap, kAllButtons)) ++hits_all;
        }
        check_eq_i64(hits_all, 4, "反例：五键同按时四档全命中（互斥不足以让选档唯一）");

        // (c) 兜底必须是确定的：取面板顺序里第一个命中的档。
        check_eq_i64(ttbox::core::aim::aim_profile_match(mp, kAllButtons), 0,
                     "多键同按 -> 取面板顺序第一个命中的档（确定，不随内部遍历漂）");

        // (d) 顺序优先的直接证据：同一组键位、顺序倒置 ⇒ 生效的档变了。
        //     别把这条当成 bug —— 它就是"面板顺序优先"的定义，也是用户唯一能控制的排序手段。
        ttbox::core::aim::MouseProfile fwd;  // 面板上左键那张卡在前
        fwd.aim_profiles = {mk(0x01, 0x00, 0), mk(0x10, 0x00, 0)};
        ttbox::core::aim::MouseProfile rev;  // 面板上侧2 那张卡在前
        rev.aim_profiles = {mk(0x10, 0x00, 0), mk(0x01, 0x00, 0)};
        check_eq_i64(ttbox::core::aim::aim_profile_match(fwd, 0x11), 0, "顺序优先：正序取第 0 张");
        check_eq_i64(ttbox::core::aim::aim_profile_match(rev, 0x11), 0, "顺序优先：倒序取第 0 张");
        check_eq_i64(fwd.aim_profiles[0].hotkey, 0x01, "顺序优先：正序生效的是左键那张卡");
        check_eq_i64(rev.aim_profiles[0].hotkey, 0x10, "顺序优先：倒序生效的是侧2 那张卡");

        // 互斥判定本身（面板保存校验用同一个函数，两处口径不能漂）
        check(ttbox::core::aim::aim_profiles_overlap(mk(0x02, 0x00, 0), mk(0x02, 0x00, 0)),
              "重叠判定：同主键 -> 重叠");
        check(ttbox::core::aim::aim_profiles_overlap(mk(0x02, 0x00, 0), mk(0x04, 0x02, 1)),
              "重叠判定：副键撞别人的主键 -> 重叠");
        check(ttbox::core::aim::aim_profiles_overlap(mk(0x02, 0x08, 0), mk(0x01, 0x08, 0)),
              "重叠判定：副键互撞 -> 重叠");
        check(!ttbox::core::aim::aim_profiles_overlap(mk(0x02, 0x00, 0), mk(0x01, 0x00, 0)),
              "重叠判定：主键不同 -> 不重叠");
        check(!ttbox::core::aim::aim_profiles_overlap(mk(0x02, 0x08, 0), mk(0x01, 0x10, 0)),
              "重叠判定：主副都不撞 -> 不重叠");
    }

    // ================= 5. 输出闸门键位并集 =================
    // 闸门若只看某一档，其它档的按键报告会被整条拦掉（表现为"换个键就不瞄了"）。
    {
        ttbox::core::aim::MouseProfile mp;
        mp.aim_profiles = {mk(0x02, 0x00, 0), mk(0x01, 0x00, 0), mk(0x04, 0x08, 1)};
        const uint16_t mask = ttbox::core::aim::aim_hotkey_mask(mp);
        check_eq_i64(mask, 0x02 | 0x01 | 0x04 | 0x08, "键位并集 = 各档主副键的按位或");
        for (const auto& ap : mp.aim_profiles) {
            const uint16_t need = static_cast<uint16_t>(ap.hotkey) | static_cast<uint16_t>(ap.hotkey2);
            check((mask & need) == need, "并集覆盖该档全部键位");
        }

        ttbox::core::aim::MouseProfile empty;
        empty.aim_profiles.clear();
        check_eq_i64(ttbox::core::aim::aim_hotkey_mask(empty), 0,
                     "数组为空时并集为 0（触发 fail-closed，不猜默认键）");
    }

    // ================= 6. 多档序列化往返 =================
    {
        RuntimeProfile p;
        auto a = mk(0x02, 0x00, 0);
        a.offset_x = 0.42f;
        a.offset_y = 0.31f;
        a.sensitivity = 1.35f;
        a.fov_scale = 0.72f;
        a.zoom_scale = 2.873f;   // V3 阶段 2：2 倍镜实测真实倍率
        a.gain_px_per_count = 0.343f;  // V3 阶段 5：本档实测 px/count（前馈换算用）
        a.class_filter = {0, 2};
        ClassOffset co;
        co.class_id = 2; co.offset_x = 0.25f; co.offset_y = 0.15f; co.priority = 3;
        a.class_offsets.push_back(co);

        auto b = mk(0x01, 0x08, 1);
        b.offset_x = 0.55f;
        b.offset_y = 0.66f;
        b.sensitivity = 0.85f;
        b.fov_scale = 0.40f;
        b.zoom_scale = 8.674f;   // V3 阶段 2：6 倍镜实测真实倍率
        b.class_filter = {1};

        p.mouse.aim_profiles = {a, b};

        const JsonValue j = p.to_json();
        const RuntimeProfile q = RuntimeProfile::from_json(j);

        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles.size()), 2, "往返：档数不丢");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey, 0x02, "往返：档0 主键");
        check_eq_f(q.mouse.aim_profiles[0].offset_x, 0.42f, "往返：档0 offset_x");
        check_eq_f(q.mouse.aim_profiles[0].offset_y, 0.31f, "往返：档0 offset_y");
        check_eq_f(q.mouse.aim_profiles[0].sensitivity, 1.35f, "往返：档0 移动倍率");
        check_eq_f(q.mouse.aim_profiles[0].fov_scale, 0.72f, "往返：档0 FOV 倍率");
        check_eq_f(q.mouse.aim_profiles[0].zoom_scale, 2.873f, "往返：档0 倍镜倍率");
        check_eq_f(q.mouse.aim_profiles[0].gain_px_per_count, 0.343f, "往返：档0 px/count");
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles[0].class_filter.size()), 2,
                     "往返：档0 类别过滤个数");
        check_eq_i64(q.mouse.aim_profiles[0].class_filter[1], 2, "往返：档0 类别过滤内容");
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles[0].class_offsets.size()), 1,
                     "往返：档0 类别偏移个数");
        check_eq_f(q.mouse.aim_profiles[0].class_offsets[0].offset_y, 0.15f, "往返：档0 类别偏移值");
        check_eq_i64(q.mouse.aim_profiles[0].class_offsets[0].priority, 3, "往返：档0 类别偏移优先级");

        check_eq_i64(q.mouse.aim_profiles[1].hotkey, 0x01, "往返：档1 主键");
        check_eq_i64(q.mouse.aim_profiles[1].hotkey2, 0x08, "往返：档1 副键");
        check_eq_i64(q.mouse.aim_profiles[1].hotkey_mode, 1, "往返：档1 触发方式 = all");
        check_eq_f(q.mouse.aim_profiles[1].fov_scale, 0.40f, "往返：档1 FOV 倍率");
        check_eq_f(q.mouse.aim_profiles[1].zoom_scale, 8.674f, "往返：档1 倍镜倍率");
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles[1].class_filter.size()), 1,
                     "往返：档1 类别过滤个数");

        // 平铺镜像 = 第 0 档（只写不读，供旧版 Core / 外部工具读懂配置）
        const JsonValue* m = j.find("mouse");
        check(m && m->is_object(), "序列化：有 mouse 段");
        if (m && m->is_object()) {
            const JsonValue* fh = m->find("aim_hotkey");
            const JsonValue* fh2 = m->find("aim_hotkey2");
            check(fh && fh->is_number() && static_cast<uint8_t>(fh->as_number()) == 0x02,
                  "序列化：平铺 aim_hotkey = 第 0 档主键");
            check(fh2 && fh2->is_number() && static_cast<uint8_t>(fh2->as_number()) == 0x00,
                  "序列化：平铺 aim_hotkey2 = 第 0 档副键");
        }
    }

    // ================= 7. 老配置（只有平铺 key）合成单档 =================
    // OTA 不覆盖 config.d ⇒ 板上配置里没有 aim_profiles，这条路必须成立，
    // 否则升级后所有老设备的自瞄热键全部失效。
    {
        const RuntimeProfile q = from_text(
            R"({"mouse":{"enabled":true,"aim_hotkey":1,"aim_hotkey2":8,)"
            R"("aim_hotkey_mode":"all","offset_x":0.30,"offset_y":0.70},)"
            R"("inference":{"class_filter":[0,2]}})",
            "老配置");

        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles.size()), 1, "老配置 -> 合成 1 档");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey, 0x01, "老配置 -> 档0 主键 = 平铺 aim_hotkey");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey2, 0x08, "老配置 -> 档0 副键 = 平铺 aim_hotkey2");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey_mode, 1,
                     "老配置 -> 档0 触发方式 = 平铺 aim_hotkey_mode");
        check_eq_f(q.mouse.aim_profiles[0].offset_x, 0.30f, "老配置 -> 档0 X 偏移 = 平铺 offset_x");
        check_eq_f(q.mouse.aim_profiles[0].offset_y, 0.70f, "老配置 -> 档0 Y 偏移 = 平铺 offset_y");
        // 倍率必须是 1.0（= 不额外缩放），否则升级后画面手感会凭空变一档。
        check_eq_f(q.mouse.aim_profiles[0].sensitivity, 1.0f, "老配置 -> 档0 移动倍率 = 1.0");
        check_eq_f(q.mouse.aim_profiles[0].fov_scale, 1.0f, "老配置 -> 档0 FOV 倍率 = 1.0");
        // 类别偏移留空 = 沿用全局表（否则老配置的 class_offsets 会失效）
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles[0].class_offsets.size()), 0,
                     "老配置 -> 档0 类别偏移留空（沿用全局）");
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles[0].class_filter.size()), 2,
                     "老配置 -> 档0 类别过滤继承全局 inference.class_filter");
        check_eq_i64(q.mouse.aim_profiles[0].class_filter[1], 2,
                     "老配置 -> 继承的类别内容正确");

        // 老配置里完全没有鼠标热键 key 时，取与结构体默认一致的值。
        const RuntimeProfile bare = from_text(R"({"mouse":{"enabled":true}})", "极简老配置");
        check_eq_i64(bare.mouse.aim_profiles[0].hotkey, 0x02, "缺 key 的老配置 -> 主键回落右键默认");
    }

    // ================= 8. 数组优先于平铺 key =================
    // 两个来源同时存在且矛盾时必须用数组，否则"改了不生效"会再次复活。
    {
        const RuntimeProfile q = from_text(
            R"({"mouse":{"enabled":true,"aim_hotkey":1,)"
            R"("aim_profiles":[{"hotkey":4,"fov_scale":0.5}]}})",
            "数组优先");
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles.size()), 1, "数组优先 -> 档数=1");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey, 4, "数组优先 -> 用数组里的主键，不用平铺 key");
        check_eq_f(q.mouse.aim_profiles[0].fov_scale, 0.5f, "数组优先 -> 档字段读到");
    }

    // ================= 9. 空数组同样回退合成 =================
    {
        const RuntimeProfile q = from_text(
            R"({"mouse":{"enabled":true,"aim_hotkey":16,"aim_profiles":[]}})", "空数组");
        check_eq_i64(static_cast<int64_t>(q.mouse.aim_profiles.size()), 1,
                     "空数组 -> 回退合成单档（不留下空档表）");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey, 16, "空数组 -> 合成档读到平铺主键");
    }

    // ================= 10. 档内缺字段时逐项回落默认 =================
    {
        const RuntimeProfile q = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8}]}})", "档内缺字段");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey, 8, "档内缺字段 -> 有值的读到");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey2, 0, "档内缺字段 -> 副键默认 0");
        check_eq_i64(q.mouse.aim_profiles[0].hotkey_mode, 0, "档内缺字段 -> 触发方式默认 any");
        check_eq_f(q.mouse.aim_profiles[0].offset_y, 0.5f, "档内缺字段 -> 偏移默认 0.5");
        check_eq_f(q.mouse.aim_profiles[0].sensitivity, 1.0f, "档内缺字段 -> 倍率默认 1.0");
    }

    // ================= 11. V3 阶段 2：倍镜倍率的兜底 =================
    // zoom_scale 是 PID 误差的**分母**，0 / 负数 / 缺失会让误差变号或变无穷大，
    // 整条输出链直接废掉 ⇒ 解析层必须一律回退 1.0（腰射 = 不折算）。
    {
        const RuntimeProfile miss = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8}]}})", "倍镜倍率缺失");
        check_eq_f(miss.mouse.aim_profiles[0].zoom_scale, 1.0f,
                   "倍镜倍率缺失 -> 回退 1.0（腰射，不做折算）");

        const RuntimeProfile zero = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8,"zoom_scale":0}]}})", "倍镜倍率 0");
        check_eq_f(zero.mouse.aim_profiles[0].zoom_scale, 1.0f,
                   "倍镜倍率 0 -> 回退 1.0（防除零）");

        const RuntimeProfile neg = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8,"zoom_scale":-3.5}]}})",
            "倍镜倍率负数");
        check_eq_f(neg.mouse.aim_profiles[0].zoom_scale, 1.0f,
                   "倍镜倍率负数 -> 回退 1.0（防误差变号）");
    }

    // ================= 12. V3 阶段 5：本档 px/count =================
    // 前馈换算要用"当前倍镜的 px/count"。没测过必须是 0（回退腰射 gain），
    // 负数也同样回退 0 —— 拿负数去换算会把抖动扣成反向，比不扣更糟。
    {
        const RuntimeProfile miss = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8}]}})", "本档 px/count 缺失");
        check_eq_f(miss.mouse.aim_profiles[0].gain_px_per_count, 0.0f,
                   "本档 px/count 缺失 -> 0（未标定，回退腰射 gain）");

        const RuntimeProfile neg = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8,"gain_px_per_count":-1.2}]}})",
            "本档 px/count 负数");
        check_eq_f(neg.mouse.aim_profiles[0].gain_px_per_count, 0.0f,
                   "本档 px/count 负数 -> 0（防抖动被反向扣除）");

        const RuntimeProfile ok = from_text(
            R"({"mouse":{"enabled":true,"aim_profiles":[{"hotkey":8,"gain_px_per_count":0.343}]}})",
            "本档 px/count 正常");
        check_eq_f(ok.mouse.aim_profiles[0].gain_px_per_count, 0.343f,
                   "本档 px/count 正常值往返一致");
    }

    if (g_fails == 0) std::printf("test_aim_profiles: PASS\n");
    else std::printf("test_aim_profiles: %d FAILED\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
