// test_mouse_control_client.cpp — 官方 MOVE 协议编码测试
#include <cstdio>
#include <algorithm>
#include <vector>
#include "output/MouseControlClient.hpp"

int main() {
    const auto p = ttbox::core::output::MouseControlClient::encode_move(0x01020304u, 10, -5, 0);
    const std::vector<unsigned char> expected = {
        0x50, 0x4f, 0x01, 0x04, 0x04, 0x03, 0x02, 0x01,
        0x0a, 0x00, 0x00, 0x00, 0xfb, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00};
    if (p.size() != expected.size() || !std::equal(p.begin(), p.end(), expected.begin())) {
        std::fprintf(stderr, "MOVE packet mismatch\n");
        return 1;
    }
    std::printf("MOVE packet PASS: 20 bytes\n");
    return 0;
}
