// test_inject_clock.cpp — AI 位移"自有时钟"投递策略的单元测试（纯逻辑，无线程）。
//
// 覆盖：单份报告限幅、余量顺延、余量上限（陈旧量丢弃）、滚轮不叠加、
//       报告构造（只写 buttons/X/Y/wheel，绝不碰别的字节）、边界与失败路径。
//
// ★ 一律用 CHECK / CHECK_TRUE 宏计数，**禁裸 assert** —— Release(-DNDEBUG) 下裸 assert
//   会被整段编译掉，测试会"一个检查都不做、永远退出 0"（本仓库栽过一次）。
#include "../inject_clock.hpp"
#include "../hid_report_layout.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ttbox_usbproxy;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_TRUE(cond) CHECK(cond)

// 十六进制字符串 → 字节（允许空格/换行分隔）。
static std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> out;
    int hi = -1;
    for (const char* p = s; *p; ++p) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<uint8_t>((hi << 4) | v)); hi = -1; }
    }
    return out;
}

// ── 现场那只鼠标（373b:10c9 Compx Nearlink dongle）的形状。
//    按板端 [LAYOUT] 实测口径复刻，**逐字对齐**那四行日志：
//      「接口 2 描述符 87 字节：解析成功，1 份报告，report_id 前缀=无」
//      「位移报告 rid=0：X@bit8/16bit Y@bit24/16bit wheel=有，报告共 7 字节」
//      「安全校验：X/Y 与 buttons 不重叠，与 wheel 不重叠」
//      「按键报告 rid=0：buttons@bit0/5bit」
//    即 bits 0..7 = 5 键 + 3 bit padding、8..23 = X(int16)、24..39 = Y(int16)、
//    40..47 = wheel、48..55 = 一个 8 bit 尾字段（解析器对所有 Input 项都推进游标，
//    所以 total_bits=56 → 7 字节）。尾字段我们只置 0、不解释，物理报告回灌时原样保留。
//    ★ 真描述符本体 87 字节（usbfs 被 usb-proxy 独占，从板上读不出来），
//      这里按"解析后形状完全相同"复刻成 68 字节 —— 断言只看解析结果，不看字节数。
static const char* kFieldMouseDescHex =
    "05 01 09 02 a1 01 09 01 a1 00 "
    "05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02 95 01 75 03 81 01 "
    "05 01 09 30 09 31 15 81 25 7f 75 10 95 02 81 06 "
    "09 38 15 81 25 7f 75 08 95 01 81 06 "
    "95 01 75 08 81 01 c0 c0";

// 解析现场鼠标的描述符形状（断言解析成功且可用）。
static HidMouseDescriptor field_mouse() {
    std::vector<uint8_t> d = unhex(kFieldMouseDescHex);
    HidMouseDescriptor desc;
    CHECK_TRUE(hid_parse_report_descriptor(d.data(), d.size(), &desc));
    CHECK_TRUE(desc.usable());
    return desc;
}

// ── 用例 1：限幅与余量顺延 ────────────────────────────────────────
static void test_plan_basic() {
    std::printf("\n[1] plan：限幅 + 余量顺延\n");
    const InjectClockConfig cfg;   // 默认 step 96 / backlog 768

    InjectStep a = inject_clock_plan(cfg, 40, -30, 0);
    CHECK(a.dx == 40); CHECK(a.dy == -30);
    CHECK(a.rest_x == 0); CHECK(a.rest_y == 0);
    CHECK(a.dropped_x == 0 && a.dropped_y == 0);

    // 一步放不下：投 96，余 768 顺延，超过上限的 136 丢弃
    InjectStep b = inject_clock_plan(cfg, 1000, 0, 0);
    CHECK(b.dx == 96);
    CHECK(b.rest_x == 768);
    CHECK(b.dropped_x == 1000 - 96 - 768);
    CHECK(b.dropped_x == 136);

    // 负方向对称
    InjectStep c = inject_clock_plan(cfg, 0, -1000, 0);
    CHECK(c.dy == -96);
    CHECK(c.rest_y == -768);
    CHECK(c.dropped_y == -136);

    // 恰好在上限边界：96 全额投、97 投 96 余 1
    InjectStep d = inject_clock_plan(cfg, 96, 96, 0);
    CHECK(d.dx == 96); CHECK(d.rest_x == 0);
    InjectStep e = inject_clock_plan(cfg, 97, -97, 0);
    CHECK(e.dx == 96); CHECK(e.rest_x == 1);
    CHECK(e.dy == -96); CHECK(e.rest_y == -1);
}

// ── 用例 2：滚轮绝不叠加 ─────────────────────────────────────────
static void test_plan_wheel() {
    std::printf("\n[2] plan：滚轮一格就是一次事件，绝不叠加\n");
    const InjectClockConfig cfg;
    InjectStep a = inject_clock_plan(cfg, 0, 0, 5);
    CHECK(a.wheel == 1);
    CHECK(a.rest_wheel == 1);       // 余量上限 = 1 ⇒ 只顺延一格
    CHECK(a.dropped_wheel == 3);    // 其余丢弃（不能把 5 格滚轮灌给游戏）

    InjectStep b = inject_clock_plan(cfg, 0, 0, -5);
    CHECK(b.wheel == -1);
    CHECK(b.rest_wheel == -1);
    CHECK(b.dropped_wheel == -3);

    InjectStep c = inject_clock_plan(cfg, 0, 0, 0);
    CHECK(c.wheel == 0 && c.rest_wheel == 0 && c.dropped_wheel == 0);
}

// ── 用例 3：报告构造（现场鼠标形状）──────────────────────────────
static void test_build_field_mouse() {
    std::printf("\n[3] build：按真实布局构造，只写 buttons/X/Y/wheel\n");
    HidMouseDescriptor desc = field_mouse();
    const HidReportLayout* lay = desc.xy_layout();
    CHECK_TRUE(lay != nullptr);
    CHECK(lay->total_bytes() == 7);
    // 钉住字段落位 —— 这就是板端 [LAYOUT] 那两行日志的内容
    CHECK(lay->buttons.present && lay->buttons.bit_offset == 0 && lay->buttons.bit_size == 5);
    CHECK(lay->x.present && lay->x.bit_offset == 8 && lay->x.bit_size == 16);
    CHECK(lay->y.present && lay->y.bit_offset == 24 && lay->y.bit_size == 16);
    CHECK(lay->wheel.present && lay->wheel.bit_offset == 40 && lay->wheel.bit_size == 8);

    uint8_t buf[16];
    uint32_t len = 0;
    InjectStep st = inject_clock_plan(InjectClockConfig{}, 40, -30, 0);
    CHECK_TRUE(inject_clock_build_report(desc, 0x1F, st, buf, sizeof(buf), &len));
    CHECK(len == 7);
    std::printf("   报告：%02x %02x %02x %02x %02x %02x %02x\n",
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    // ★ 防回归（切枪事故的墓碑）：滚轮字节必须是 0，绝不能出现"每帧一个滚轮事件"
    CHECK(buf[5] == 0x00);
    // 按键掩码写在 bit0/5（0x1F 全亮 ⇒ 落位后仍是 0x1F，padding 位为 0）
    CHECK(buf[0] == 0x1F);
    // X/Y 小端 16 位：40 = 0x0028，-30 = 0xFFE2
    CHECK(buf[1] == 0x28); CHECK(buf[2] == 0x00);
    CHECK(buf[3] == 0xE2); CHECK(buf[4] == 0xFF);

    // 回读一致
    int32_t rx = 0, ry = 0;
    CHECK_TRUE(hid_field_read_signed(buf, len, lay->x, &rx));
    CHECK_TRUE(hid_field_read_signed(buf, len, lay->y, &ry));
    CHECK(rx == 40); CHECK(ry == -30);
    uint32_t mask = 0;
    CHECK_TRUE(hid_field_read_mask(buf, len, lay->buttons, &mask));
    CHECK(mask == 0x1F);

    // 零位移报告也能构造（就是"没移动"），且不会碰 wheel/buttons
    InjectStep zero = inject_clock_plan(InjectClockConfig{}, 0, 0, 0);
    uint8_t z[16];
    uint32_t zlen = 0;
    CHECK_TRUE(inject_clock_build_report(desc, 0x00, zero, z, sizeof(z), &zlen));
    CHECK(zlen == 7);
    for (int i = 0; i < 7; ++i) CHECK(z[i] == 0x00);
}

// ── 用例 4：限幅后的步长在 int16 字段里可表达 ─────────────────────
static void test_step_fits_int16() {
    std::printf("\n[4] plan：步长与 HID 字段位宽相容\n");
    HidMouseDescriptor desc = field_mouse();
    // 极端积压：一次 100 万 count 也只会投 96
    InjectStep st = inject_clock_plan(InjectClockConfig{}, 1000000, -1000000, 0);
    uint8_t buf[16];
    uint32_t len = 0;
    CHECK_TRUE(inject_clock_build_report(desc, 0, st, buf, sizeof(buf), &len));
    const HidReportLayout* lay = desc.xy_layout();
    int32_t rx = 0;
    CHECK_TRUE(hid_field_read_signed(buf, len, lay->x, &rx));
    CHECK(rx == 96);
    CHECK(st.dx == 96 && st.dy == -96);
}

// ── 用例 5：带 report_id 前缀的形状 ──────────────────────────────
static void test_build_with_report_id() {
    std::printf("\n[5] build：带 report_id 前缀时首字节写 rid\n");
    // 两份报告：rid=1 按键（3 位）、rid=2 位移（按钮 3 位 + X/Y int8 + wheel）
    std::vector<uint8_t> d = unhex(
        "05 01 09 02 a1 01 "
        "85 01 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01 81 02 95 05 75 01 81 03 c0 "
        "85 02 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01 81 02 95 05 75 01 81 03 "
        "05 01 09 30 09 31 15 81 25 7f 75 08 95 02 81 06 09 38 15 81 25 7f 75 08 95 01 81 06 c0 c0");
    HidMouseDescriptor desc;
    CHECK_TRUE(hid_parse_report_descriptor(d.data(), d.size(), &desc));
    CHECK_TRUE(desc.uses_report_ids);
    CHECK_TRUE(desc.usable());

    const HidReportLayout* xy = desc.xy_layout();
    CHECK_TRUE(xy != nullptr && xy->report_id == 2);

    uint8_t buf[16];
    uint32_t len = 0;
    InjectStep st = inject_clock_plan(InjectClockConfig{}, 5, -5, 0);
    CHECK_TRUE(inject_clock_build_report(desc, 0x03, st, buf, sizeof(buf), &len));
    CHECK(len == static_cast<uint32_t>(xy->total_bytes()) + 1u);
    CHECK(buf[0] == 0x02);   // report_id 前缀

    // 前缀之后的位移字段：X=-5? 不，X=5 → 0x05，Y=-5 → 0xFB
    int32_t rx = 0, ry = 0;
    CHECK_TRUE(hid_field_read_signed(buf + 1, len - 1, xy->x, &rx));
    CHECK_TRUE(hid_field_read_signed(buf + 1, len - 1, xy->y, &ry));
    CHECK(rx == 5); CHECK(ry == -5);
}

// ── 用例 6：按键写入只动那几位（与 hid_field_write_mask 对称）─────
static void test_write_mask_is_surgical() {
    std::printf("\n[6] build：按键掩码只写自己那几位\n");
    HidMouseDescriptor desc = field_mouse();
    uint8_t buf[16];
    uint32_t len = 0;
    // 掩码 0xF0（高位不该进 5 位字段）
    InjectStep st = inject_clock_plan(InjectClockConfig{}, 0, 0, 0);
    CHECK_TRUE(inject_clock_build_report(desc, 0xF0, st, buf, sizeof(buf), &len));
    CHECK(buf[0] == 0x10);   // 只保留低 5 位 → 0x10（bit4 = 第 5 键）

    // 逐位验证：每个按钮位单独写都对得上
    for (int b = 0; b < 5; ++b) {
        const uint8_t one = static_cast<uint8_t>(1u << b);
        CHECK_TRUE(inject_clock_build_report(desc, one, st, buf, sizeof(buf), &len));
        uint32_t got = 0;
        CHECK_TRUE(hid_field_read_mask(buf, len, desc.xy_layout()->buttons, &got));
        CHECK(got == one);
    }
}

// ── 用例 7：失败路径必须拒（不能凑合发一份错的）──────────────────
static void test_build_rejects() {
    std::printf("\n[7] build：不可用/装不下 ⇒ 返回 false\n");
    HidMouseDescriptor bad;   // parsed = false
    InjectStep st;
    uint8_t buf[16];
    uint32_t len = 0;
    CHECK(!inject_clock_build_report(bad, 0, st, buf, sizeof(buf), &len));
    CHECK(!inject_clock_build_report(field_mouse(), 0, st, nullptr, 4, &len));
    CHECK(!inject_clock_build_report(field_mouse(), 0, st, buf, 4, &len));   // cap 太小
    CHECK(!inject_clock_build_report(field_mouse(), 0, st, buf, sizeof(buf), nullptr));

    // 纯键盘描述符（没有 X/Y 也没有 buttons）⇒ 拒
    std::vector<uint8_t> kb = unhex("05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 c0");
    HidMouseDescriptor kbd;
    CHECK_TRUE(hid_parse_report_descriptor(kb.data(), kb.size(), &kbd));
    CHECK(!kbd.usable());
    CHECK(!inject_clock_build_report(kbd, 0, st, buf, sizeof(buf), &len));
}

// 依次跑全部用例，汇总检查项与失败数，返回进程退出码。
int main() {
    std::printf("=== inject_clock 单元测试 ===\n");
    test_plan_basic();
    test_plan_wheel();
    test_build_field_mouse();
    test_step_fits_int16();
    test_build_with_report_id();
    test_write_mask_is_surgical();
    test_build_rejects();
    std::printf("\n检查项：%d，失败：%d\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "PASSED" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
