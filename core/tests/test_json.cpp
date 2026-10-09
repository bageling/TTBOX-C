// test_json.cpp — JSON 解析器的嵌套深度护栏
//
// 背景（2026-09-23 全仓审查复核 #20）：parse_value ↔ parse_object/parse_array 是纯递归下降，
// 此前没有任何深度计数 ⇒ 连续嵌套的 '[' 会按输入长度线性吃栈。IPC 单行实际可到约 69.6KB
// （read_line 的上限判断排在 find('\n') 之后），即约 6.9 万层，远超默认线程栈 ——
// 本机同组进程发一次请求就能打崩 Core。
//
// 反向验证：撤掉 Parser 的 kMaxDepth 检查后，下面两个"必须被拒"的用例会失败（解析成功），
// 第三个"合理深度"用例仍应通过。只跑"合理深度"那条是空的。
#include <string>

#include "common/Json.hpp"
#include "test_util.hpp"

TEST(json_parse_rejects_excessive_array_nesting) {
    // 400 层 > kMaxDepth(256)：必须被拒，而不是继续吃栈。
    std::string deep(400, '[');
    deep += std::string(400, ']');
    auto r = ttbox::core::json_parse(deep);
    CHECK(!r.ok);
}

TEST(json_parse_rejects_excessive_object_nesting) {
    // 对象是另一条递归路径（parse_object），单独钉一次。
    std::string s;
    for (int i = 0; i < 400; ++i) s += "{\"a\":";
    s += "1";
    for (int i = 0; i < 400; ++i) s += "}";
    auto r = ttbox::core::json_parse(s);
    CHECK(!r.ok);
}

TEST(json_parse_accepts_reasonable_nesting) {
    // 护栏另一侧：真实请求的深度必须照常通过，避免把正常配置/状态请求一起挡了。
    // 32 层远超实际用途（配置深合并也就几层），留足余量。
    const int kDepth = 32;
    std::string s(kDepth, '[');
    s += std::string(kDepth, ']');
    auto r = ttbox::core::json_parse(s);
    CHECK(r.ok);

    std::string obj;
    for (int i = 0; i < kDepth; ++i) obj += "{\"a\":";
    obj += "1";
    for (int i = 0; i < kDepth; ++i) obj += "}";
    auto r2 = ttbox::core::json_parse(obj);
    CHECK(r2.ok);
}

TEST(json_parse_depth_error_is_reported) {
    // 拒绝时要给出可诊断的错误（而不是静默空值），否则线上只会看到"解析失败"没有线索。
    std::string deep(300, '[');
    deep += std::string(300, ']');
    auto r = ttbox::core::json_parse(deep);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}
