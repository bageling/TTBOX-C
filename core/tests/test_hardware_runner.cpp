// test_hardware_runner.cpp — 仅验证 HardwareRunner 接口可构建，不打开板端设备。
#include <cassert>
#include <memory>
#include "runtime/HardwareRunner.hpp"
#include "output/IHidOutput.hpp"
int main(){
 ttbox::core::HardwareRunner r;
 ttbox::core::HardwareRunner::Params p;
 p.capture.device="/dev/video0"; p.workers.worker_cores={1};
 p.output=std::make_shared<ttbox::core::output::NullHidOutput>();
 std::string error;
    (void)r.initialize(p, &error); // 仅检查接口可调用，不打开设备
 return 0;
}
