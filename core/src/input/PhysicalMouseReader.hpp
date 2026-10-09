// PhysicalMouseReader.hpp — 真实物理鼠标 evdev 输入读取。
// 只读 /dev/input/eventN，不模拟鼠标；输出按钮位图供 AimThread 热键门控使用。
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include "common/Paths.hpp"   // A-PATH-5：mouse event.sock 默认单点真源
namespace ttbox::core::input {
class PhysicalMouseReader {
public:
    // 析构时自动 stop
    ~PhysicalMouseReader(){stop();}
    // 启动读取：device 为空走 usb-proxy event.sock，否则打开 evdev；失败返回 false
    bool start(const std::string& device="", std::string* error=nullptr);
    // 停止读取并 join 收尾线程
    void stop();
    // 当前按键位图（原子读取）
    uint16_t buttons() const{return buttons_.load(std::memory_order_acquire);}
    // 取走并清零累计 X 位移
    int32_t rel_x(){return rel_x_.exchange(0,std::memory_order_acq_rel);}
    // 取走并清零累计 Y 位移
    int32_t rel_y(){return rel_y_.exchange(0,std::memory_order_acq_rel);}
    // 是否运行中
    bool running() const{return running_.load();}
    // 暴露按键位原子量，供他处直接读（热键门控）
    std::atomic<uint16_t>* button_source(){return &buttons_;}
    // 当前设备路径
    std::string device() const{return device_;}
    // usb-proxy 按键事件通道。默认使用 TTBOX 自己的运行目录，避免读到其它服务的 event.sock。
    void set_event_socket_path(const std::string& path){ if(!path.empty()) event_socket_path_=path; }
private:
    // evdev 读取线程主体
    void loop();
    // usb-proxy event.sock 读取线程主体
    void event_socket_loop();
    // 扫描 /dev/input/event* 找到带相对位移与鼠标键的设备
    bool find_device(std::string* out) const;
    // 只负责建连/订阅/收 ACK；不断开时由 event_socket_loop 持有。
    bool open_event_socket(std::string* error);
    // 连接/订阅 event.sock（失败按 300ms×12 重试），成功后拉起事件线程
    bool start_event_socket(std::string* error);
    std::string device_;
    std::string event_socket_path_=paths::kMouseEventSocketDefault;
    // fd_=evdev 句柄；event_fd_=event.sock 句柄；均由读取线程与 stop() 共享
    int fd_=-1; int event_fd_=-1; std::atomic<bool> running_{false}; std::thread thread_; std::thread event_thread_;
    std::atomic<uint16_t> buttons_{0};
 std::atomic<int32_t> rel_x_{0},rel_y_{0};
};
}
