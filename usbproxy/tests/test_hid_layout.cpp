// test_hid_layout.cpp — hid_report_layout 自测（纯逻辑，不依赖 libusb/lua/jsoncpp）
//
// 跑法（WSL 或 msys 都行）：
//   g++ -std=c++17 -O2 -Wall -Wextra test_hid_layout.cpp ../hid_report_layout.cpp -o t
//   ./t
//
// 为什么不用裸 assert：Release（-DNDEBUG）下 `assert` 整段被编译掉 ⇒ 测试文件一个检查都
// 不做、永远退出 0（本项目已在 test_stats_window.cpp 上栽过一次：缺陷复现了它照样打印
// PASSED）。这里一律走 CHECK 宏，任何优化级别下都真的执行。
#include "../hid_report_layout.hpp"
#include "../hidraw_source.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ttbox_usbproxy;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// 十六进制字符串 → 字节（允许空格/换行分隔）
static std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> out;
    int hi = -1;
    for (const char* p = s; *p; ++p) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

static HidMouseDescriptor parse_or_die(const char* label, const char* hex) {
    std::vector<uint8_t> d = unhex(hex);
    HidMouseDescriptor desc;
    const bool ok = hid_parse_report_descriptor(d.data(), d.size(), &desc);
    std::printf("· %s（%zu 字节）→ parsed=%d 报告数=%d\n", label, d.size(), ok ? 1 : 0,
                desc.layout_count);
    CHECK(ok);
    return desc;
}

static void dump_layout(const HidReportLayout& l) {
    std::printf("    rid=%d total=%dbit(%d字节) buttons=%s@%d/%d X=%s@%d/%d Y=%s@%d/%d wheel=%s@%d/%d\n",
                l.report_id, l.total_bits, l.total_bytes(),
                l.buttons.present ? "有" : "无", l.buttons.bit_offset, l.buttons.bit_size,
                l.x.present ? "有" : "无", l.x.bit_offset, l.x.bit_size,
                l.y.present ? "有" : "无", l.y.bit_offset, l.y.bit_size,
                l.wheel.present ? "有" : "无", l.wheel.bit_offset, l.wheel.bit_size);
}

// ── 用例 1：项目自带的合成 gadget 描述符（boot 鼠标 + 滚轮，无 report_id）──
static void test_synthetic_gadget_desc() {
    std::printf("\n[1] 合成 gadget 描述符（gadget-config.json 那份）\n");
    HidMouseDescriptor desc = parse_or_die(
        "synthetic-corsair",
        "05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02"
        "95 01 75 03 81 01 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 c0 c0");
    CHECK(desc.uses_report_ids == false);
    CHECK(desc.layout_count == 1);
    const HidReportLayout* l = desc.xy_layout();
    CHECK(l != nullptr);
    if (l == nullptr) return;
    dump_layout(*l);
    CHECK(l->report_id == 0);
    CHECK(l->buttons.present && l->buttons.bit_offset == 0 && l->buttons.bit_size == 5);
    CHECK(l->x.present && l->x.bit_offset == 8 && l->x.bit_size == 8 && l->x.is_signed);
    CHECK(l->y.present && l->y.bit_offset == 16 && l->y.bit_size == 8 && l->y.is_signed);
    CHECK(l->wheel.present && l->wheel.bit_offset == 24 && l->wheel.bit_size == 8);
    CHECK(l->total_bytes() == 4);   // 与 hid_report_length=4 对得上
    CHECK(desc.usable());
    // 安全校验：X/Y 不该与 buttons 或 wheel 的字节区间重叠
    CHECK(!hid_fields_overlap_bytes(l->x, l->buttons));
    CHECK(!hid_fields_overlap_bytes(l->y, l->buttons));
    CHECK(!hid_fields_overlap_bytes(l->x, l->wheel));
    CHECK(!hid_fields_overlap_bytes(l->y, l->wheel));
}

// ── 用例 2：现场那只鼠标（373b:10c9）的形状 ──
// 实测报告 7 字节：02 XX XX XX XX XX XX（rid=2 + 6 字节数据）
// 旧代码按罗技写死 rid=2/X@3/Y@5 ⇒ X 写到 Y、**Y 写到 wheel** ⇒ 每帧一个滚轮 ⇒ 疯狂切枪。
static const char* kFieldMouseDescHex =
    "05 01 09 02 a1 01 85 02 09 01 a1 00"
    "05 09 19 01 29 08 15 00 25 01 95 08 75 01 81 02"
    "05 01 09 30 09 31 15 81 25 7f 75 10 95 02 81 06"
    "09 38 15 81 25 7f 75 08 95 01 81 06"
    "c0 c0";

static void test_field_mouse_desc() {
    std::printf("\n[2] 现场鼠标形状（rid=2, buttons@0/8bit, X@8/16bit, Y@24/16bit, wheel@40/8bit）\n");
    HidMouseDescriptor desc = parse_or_die("field-mouse", kFieldMouseDescHex);
    CHECK(desc.uses_report_ids == true);
    const HidReportLayout* l = desc.find(2);
    CHECK(l != nullptr);
    if (l == nullptr) return;
    dump_layout(*l);
    CHECK(l->buttons.present && l->buttons.bit_offset == 0 && l->buttons.bit_size == 8);
    CHECK(l->x.present && l->x.bit_offset == 8 && l->x.bit_size == 16);
    CHECK(l->y.present && l->y.bit_offset == 24 && l->y.bit_size == 16);
    CHECK(l->wheel.present && l->wheel.bit_offset == 40 && l->wheel.bit_size == 8);
    CHECK(l->total_bytes() == 6);   // + report_id 1 字节 = 现场观测到的 7 字节
    CHECK(desc.usable());
    CHECK(!hid_fields_overlap_bytes(l->y, l->wheel));
    CHECK(!hid_fields_overlap_bytes(l->x, l->wheel));
}

// ── 用例 3：★★ 本次事故的防回归断言 ★★ ──
// 用 [2] 的布局写一帧 AI 位移，断言 **buttons 与 wheel 字节一个都没动**。
// 这条如果失败，就意味着"自瞄一动就疯狂切枪"会复发。
static void test_no_wheel_corruption() {
    std::printf("\n[3] 防回归：写 AI 位移不得碰 buttons / wheel 字节\n");
    HidMouseDescriptor desc = parse_or_die("field-mouse", kFieldMouseDescHex);
    const HidReportLayout* l = desc.find(2);
    CHECK(l != nullptr);
    if (l == nullptr) return;
    CHECK(desc.uses_report_ids);

    uint8_t report[7] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t* body = report + 1;          // 跳过 report_id
    const size_t body_len = 6;

    int32_t x0 = 0;
    int32_t y0 = 0;
    CHECK(hid_field_read_signed(body, body_len, l->x, &x0));
    CHECK(hid_field_read_signed(body, body_len, l->y, &y0));
    CHECK(x0 == 0 && y0 == 0);
    CHECK(hid_field_write_signed(body, body_len, l->x, x0 + 7));
    CHECK(hid_field_write_signed(body, body_len, l->y, y0 - 3));

    CHECK(report[1] == 0x00);            // buttons 字节：必须原样
    CHECK(report[6] == 0x00);            // ★ wheel 字节：必须原样（旧代码就是在这里写进了 Y 位移）
    int32_t x1 = 0;
    int32_t y1 = 0;
    CHECK(hid_field_read_signed(body, body_len, l->x, &x1) && x1 == 7);
    CHECK(hid_field_read_signed(body, body_len, l->y, &y1) && y1 == -3);
    std::printf("    报告写后：%02x %02x %02x %02x %02x %02x %02x（wheel 位=0）\n",
                report[0], report[1], report[2], report[3], report[4], report[5], report[6]);
}

// ── 用例 4：按键与位移分属两个 report_id（现场很可能就是这种）──
static void test_split_button_report() {
    std::printf("\n[4] 按键报告与位移报告分离（rid=1 按键 / rid=2 位移）\n");
    HidMouseDescriptor desc = parse_or_die(
        "split-rid",
        "05 01 09 02 a1 01"
        "85 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02"
        "95 01 75 03 81 01 c0"
        "85 02 09 01 a1 00 05 01 09 30 09 31 15 81 25 7f 75 10 95 02 81 06"
        "09 38 15 81 25 7f 75 08 95 01 81 06 c0"
        "c0");
    CHECK(desc.uses_report_ids);
    CHECK(desc.layout_count == 2);
    const HidReportLayout* btn = desc.button_layout();
    const HidReportLayout* xy = desc.xy_layout();
    CHECK(btn != nullptr && btn->report_id == 1);
    CHECK(xy != nullptr && xy->report_id == 2);
    if (btn == nullptr || xy == nullptr) return;
    dump_layout(*btn);
    dump_layout(*xy);
    CHECK(btn->has_buttons() && !btn->has_xy());   // 按键报告里没有 X/Y
    CHECK(xy->has_xy() && !xy->has_buttons());     // 位移报告里没有 buttons

    // 按键报告：掩码按真实位置读
    uint8_t btn_report[2] = {0x01, 0x05};   // rid=1, buttons=5（左+中）
    uint32_t mask = 0;
    CHECK(hid_field_read_mask(btn_report + 1, 1, btn->buttons, &mask));
    CHECK(mask == 5);

    // 位移报告：写位移不能碰按键那份报告
    uint8_t move_report[7] = {0x02, 0, 0, 0, 0, 0, 0};
    CHECK(hid_field_write_signed(move_report + 1, 6, xy->x, 11));
    CHECK(move_report[6] == 0);   // wheel 仍未被动
    CHECK(btn_report[1] == 0x05);
}

// ── 用例 5：boot 三键鼠标（无 rid，4 字节报告）──
static void test_boot_mouse() {
    std::printf("\n[5] boot 鼠标描述符（buttons 只占 3 位）\n");
    HidMouseDescriptor desc = parse_or_die(
        "boot-mouse",
        "05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01 81 02"
        "95 01 75 05 81 03 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 c0 c0");
    const HidReportLayout* l = desc.xy_layout();
    CHECK(l != nullptr);
    if (l == nullptr) return;
    dump_layout(*l);
    CHECK(l->buttons.present && l->buttons.bit_size == 3);
    CHECK(l->x.bit_offset == 8 && l->y.bit_offset == 16 && l->wheel.bit_offset == 24);

    // 掩码取 3 位：bit2 置起来只读到 0b111 的部分
    uint8_t r[4] = {0x07, 0, 0, 0};
    uint32_t mask = 0;
    CHECK(hid_field_read_mask(r, 4, l->buttons, &mask) && mask == 7);
    uint8_t r2[4] = {0x2F, 0, 0, 0};   // 高 5 位是填充，不该被算进掩码
    CHECK(hid_field_read_mask(r2, 4, l->buttons, &mask) && mask == 7);
}

// ── 用例 6：畸形 / 非鼠标描述符必须被判"不可用" ──
static void test_bad_descriptors() {
    std::printf("\n[6] 畸形与不可用描述符\n");

    HidMouseDescriptor desc;
    CHECK(!hid_parse_report_descriptor(nullptr, 10, &desc));
    CHECK(!hid_parse_report_descriptor(reinterpret_cast<const uint8_t*>("x"), 0, &desc));

    // 截断：Report Size 声明了 1 字节数据，但文件到此为止
    std::vector<uint8_t> trunc = unhex("05 01 09 30 75");
    CHECK(!hid_parse_report_descriptor(trunc.data(), trunc.size(), &desc));

    // 纯键盘描述符：能解析，但没有任何 X/Y/buttons ⇒ 整体不可用
    std::vector<uint8_t> kbd = unhex(
        "05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02"
        "95 01 75 08 81 03 05 08 19 01 29 05 75 01 95 05 91 02 95 01 75 03 91 03 c0");
    const bool ok = hid_parse_report_descriptor(kbd.data(), kbd.size(), &desc);
    std::printf("    键盘描述符：parsed=%d 报告数=%d usable=%d\n", ok ? 1 : 0,
                desc.layout_count, desc.usable() ? 1 : 0);
    CHECK(ok);
    CHECK(desc.xy_layout() == nullptr);
    CHECK(desc.button_layout() == nullptr);
    CHECK(!desc.usable());
}

// ── 用例 7：字段读写边界与重叠判定 ──
static void test_field_primitives() {
    std::printf("\n[7] 字段读写边界\n");

    // 越界写：字段要 byte1..2，但报告只有 2 字节
    uint8_t small[2] = {0, 0};
    HidField f16{true, 8, 16, true};
    int32_t v = 0;
    CHECK(!hid_field_read_signed(small, 2, f16, &v));
    CHECK(!hid_field_write_signed(small, 2, f16, 42));

    // 非法位宽：一律拒（5 位 signed 字段不安全）
    HidField weird{true, 0, 5, true};
    CHECK(!hid_field_is_safe(weird));

    // 非字节对齐 8 位：2026-09-27 起支持（位窗口 RMW，邻居位必须原样保留）
    HidField misaligned{true, 3, 8, true};
    CHECK(hid_field_is_safe(misaligned));
    uint8_t rm[3] = {0xFF, 0xFF, 0xFF};
    CHECK(hid_field_write_signed(rm, 3, misaligned, 1));
    // 流 bit3=1、bit4..10=0；其余位保持 1 ⇒ byte0=0x0F、byte1=0xF8、byte2 不动
    CHECK(rm[0] == 0x0F);
    CHECK(rm[1] == 0xF8);
    CHECK(rm[2] == 0xFF);
    CHECK(hid_field_read_signed(rm, 3, misaligned, &v) && v == 1);

    // 饱和钳制：int8 写 300 → 127，写 -300 → -128
    uint8_t r[1] = {0};
    HidField f8{true, 0, 8, true};
    CHECK(hid_field_write_signed(r, 1, f8, 300) && r[0] == 127);
    CHECK(hid_field_write_signed(r, 1, f8, -300) && r[0] == 0x80);
    int32_t back = 0;
    CHECK(hid_field_read_signed(r, 1, f8, &back) && back == -128);

    // 12 位饱和钳制：写 3000 → 2047，写 -3000 → -2048
    HidField f12{true, 8, 12, true};
    uint8_t r12[3] = {0, 0, 0};
    CHECK(hid_field_write_signed(r12, 3, f12, 3000));
    CHECK(hid_field_read_signed(r12, 3, f12, &back) && back == 2047);
    CHECK(hid_field_write_signed(r12, 3, f12, -3000));
    CHECK(hid_field_read_signed(r12, 3, f12, &back) && back == -2048);

    // 16 位往返（f16 从 bit8 起 ⇒ 要 3 字节才放得下，2 字节时上面已断言被拒）
    uint8_t r2[3] = {0, 0, 0};
    CHECK(hid_field_write_signed(r2, 3, f16, -1234));   // -1234 = 0xFB2E
    CHECK(hid_field_read_signed(r2, 3, f16, &back) && back == -1234);
    CHECK(r2[0] == 0);        // 字段之前的那一字节不该被动
    CHECK(r2[1] == 0x2E);     // 小端低字节
    CHECK(r2[2] == 0xFB);     // 小端高字节

    // 重叠判定
    HidField a{true, 8, 16, true};    // byte1..2
    HidField b{true, 16, 8, false};   // byte2
    HidField c{true, 24, 8, false};   // byte3
    CHECK(hid_fields_overlap_bytes(a, b));
    CHECK(!hid_fields_overlap_bytes(a, c));
    CHECK(!hid_fields_overlap_bytes(a, HidField{}));
}

// ── 用例 8：12 位 X/Y 鼠标（罗技 G 系形状，2026-09-27 修复「有些鼠标不识别」）──
// buttons@0/5bit、pad3、X@bit8/12bit（跨 byte1..2）、Y@bit20/12bit（跨 byte2..3、
// 非字节对齐）、wheel@bit32/8bit。旧实现 is_safe 拒 12 位 ⇒ 该类鼠标注入/热键全废。
static const char* kLogitech12BitDescHex =
    "05 01 09 02 a1 01 09 01 a1 00"
    "05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02"
    "95 01 75 03 81 01"
    "05 01 09 30 09 31 16 01 f8 26 ff 07 75 0c 95 02 81 06"
    "09 38 15 81 25 7f 75 08 95 01 81 06"
    "c0 c0";

static void test_logitech_12bit_desc() {
    std::printf("\n[8] 12 位 X/Y 鼠标（罗技 G 系形状：X@8/12bit 跨字节，Y@20/12bit 非对齐）\n");
    HidMouseDescriptor desc = parse_or_die("logitech-12bit", kLogitech12BitDescHex);
    CHECK(desc.uses_report_ids == false);
    const HidReportLayout* l = desc.xy_layout();
    CHECK(l != nullptr);
    if (l == nullptr) return;
    dump_layout(*l);
    CHECK(l->buttons.present && l->buttons.bit_offset == 0 && l->buttons.bit_size == 5);
    CHECK(l->x.present && l->x.bit_offset == 8 && l->x.bit_size == 12 && l->x.is_signed);
    CHECK(l->y.present && l->y.bit_offset == 20 && l->y.bit_size == 12 && l->y.is_signed);
    CHECK(l->wheel.present && l->wheel.bit_offset == 32 && l->wheel.bit_size == 8);
    CHECK(l->total_bytes() == 5);
    CHECK(desc.usable());
    // X/Y 与 buttons/wheel 字节区间不重叠（安全闸要放行）
    CHECK(!hid_fields_overlap_bytes(l->x, l->buttons));
    CHECK(!hid_fields_overlap_bytes(l->y, l->buttons));
    CHECK(!hid_fields_overlap_bytes(l->x, l->wheel));
    CHECK(!hid_fields_overlap_bytes(l->y, l->wheel));

    // 注入路径全链路：写按键 + X + Y + wheel，读回应一致
    uint8_t rep[5] = {0, 0, 0, 0, 0};
    CHECK(hid_field_write_mask(rep, 5, l->buttons, 0x17));
    CHECK(hid_field_write_signed(rep, 5, l->x, 100));
    CHECK(hid_field_write_signed(rep, 5, l->y, -33));
    CHECK(hid_field_write_signed(rep, 5, l->wheel, -1));
    int32_t xv = 0, yv = 0, wv = 0;
    uint32_t bv = 0;
    CHECK(hid_field_read_mask(rep, 5, l->buttons, &bv) && bv == 0x17);
    CHECK(hid_field_read_signed(rep, 5, l->x, &xv) && xv == 100);
    CHECK(hid_field_read_signed(rep, 5, l->y, &yv) && yv == -33);
    CHECK(hid_field_read_signed(rep, 5, l->wheel, &wv) && wv == -1);
    // X 与 Y 共享 byte2：写 X 不得破坏 Y（-33 的 12 位补码 = 0xFDF）
    CHECK(hid_field_write_signed(rep, 5, l->x, -1));
    CHECK(hid_field_read_signed(rep, 5, l->x, &xv) && xv == -1);
    CHECK(hid_field_read_signed(rep, 5, l->y, &yv) && yv == -33);
    CHECK(hid_field_read_mask(rep, 5, l->buttons, &bv) && bv == 0x17);
    // 字节层面的直观校验：X=-1 ⇒ bit8..19 全 1；Y=-33 ⇒ bit20..31 = 0xFDF
    CHECK(rep[1] == 0xFF);            // X 低 8 位
    CHECK(rep[2] == 0xFF);            // X 高 4 位(bit16..19=1) + Y 低 4 位(bit20..23=0xF)
    CHECK(rep[3] == 0xFD);            // Y 高 8 位 = 0xFD
    CHECK(rep[4] == 0xFF);            // wheel=-1
}

// ── hid_layout_decide：1.5.57 布局学习兜底的决策表（1.5.56 事故钉死的行为）──
static void test_hid_layout_decide() {
    using A = ttbox_usbproxy::HidLayoutAction;
    std::printf("--- hid_layout_decide ---\n");

    // 已就绪 → 永远等待（不动已学好的布局）。
    CHECK(ttbox_usbproxy::hid_layout_decide(true, false, 0, 0) == A::kWait);
    CHECK(ttbox_usbproxy::hid_layout_decide(true, true, 0, 999) == A::kWait);

    // 有 hidraw 缓存 → 永远走缓存（零 USB 请求，quirk 固件安全）。
    CHECK(ttbox_usbproxy::hid_layout_decide(false, true, 0, 0) == A::kFeedPrefetch);
    CHECK(ttbox_usbproxy::hid_layout_decide(false, true, 3, 999) == A::kFeedPrefetch);

    // 没缓存、class 请求没试过 → 试一次（只此一次）。
    CHECK(ttbox_usbproxy::hid_layout_decide(false, false, 0, 0) == A::kTryClassRequest);
    CHECK(ttbox_usbproxy::hid_layout_decide(false, false, 0, 9) == A::kTryClassRequest);

    // 试过一次就不再试：quirk 固件（Nearlink dongle 板端实测）会被该请求打死，
    // 反复重试 = 反复打 Dongle。之后只能等 stall 累积 → fail-open。
    CHECK(ttbox_usbproxy::hid_layout_decide(false, false, 1, 0) == A::kWait);
    CHECK(ttbox_usbproxy::hid_layout_decide(false, false, 2, 5) == A::kWait);

    // 无进展满 10 轮（×500ms ≈ 5s）→ fail-open：开门保物理透传。
    CHECK(ttbox_usbproxy::hid_layout_decide(false, false, 1, 10) == A::kFailOpen);
    CHECK(ttbox_usbproxy::hid_layout_decide(false, false, 1, 50) == A::kFailOpen);
}

int main() {
    std::printf("=== hid_report_layout 自测（CHECK 计数，非裸 assert）===\n");
    test_synthetic_gadget_desc();
    test_field_mouse_desc();
    test_no_wheel_corruption();
    test_split_button_report();
    test_boot_mouse();
    test_bad_descriptors();
    test_field_primitives();
    test_logitech_12bit_desc();
    test_hid_layout_decide();

    std::printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("PASSED\n");
    else std::printf("FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
