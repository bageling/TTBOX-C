// test_main.cpp — 测试入口：执行所有注册用例
#include <cstdio>

#include "test_util.hpp"

int main() {
    std::printf("=== ttbox_core tests ===\n");
    int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
