// ICapture.hpp — Core 采集边界
#pragma once

#include <memory>
#include <string>
#include "common/CoreContracts.hpp"

namespace ttbox::core::capture {

// ICapture：Core 采集能力的抽象接口（打开/启动/停止/关闭 + 取最新帧）。
class ICapture {
public:
    virtual ~ICapture() = default;
    // 打开采集设备并完成资源分配。
    virtual bool open(std::string* error = nullptr) = 0;
    // 启动采集（拉流 + 采集线程）。
    virtual bool start(std::string* error = nullptr) = 0;
    // 停止采集并回收运行期资源（线程 / 流 / buffer）。
    virtual void stop() = 0;
    // 关闭设备并释放全部资源（幂等）。
    virtual void close() = 0;
    // 取当前最新帧（线程安全；shared_ptr 保活）。
    virtual std::shared_ptr<Frame> latest_frame() const = 0;
};

}  // namespace ttbox::core::capture
