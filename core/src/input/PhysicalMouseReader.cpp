// PhysicalMouseReader.cpp — 真实物理鼠标输入读取。
// 优先读取 evdev；完整 USB 透传模式下，usb-proxy 独占鼠标后回退到官方 event.sock。
#include "input/PhysicalMouseReader.hpp"
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#endif

namespace ttbox::core::input {
#if !defined(_WIN32)
namespace {
// usb-proxy event.sock 帧头魔数/版本与消息类型码
constexpr uint16_t kMagic = 0x4F50;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kSubscribeReq = 8;
constexpr uint8_t kSubscribeAck = 9;
constexpr uint8_t kStateSnapshot = 10;
constexpr uint8_t kButtonEvent = 11;
// event.sock 定长帧头
#pragma pack(push, 1)
struct Header { uint16_t magic; uint8_t version; uint8_t type; uint32_t request_id; };
#pragma pack(pop)
}
#endif

// 扫描 /dev/input/event*：找同时具备相对位移(EV_REL)与鼠标键(BTN_LEFT/RIGHT)的设备
bool PhysicalMouseReader::find_device(std::string* out) const {
#if defined(_WIN32)
 (void)out; return false;
#else
 DIR* d=opendir("/sys/class/input"); if(!d)return false; dirent* e;
 while((e=readdir(d))){
  if(strncmp(e->d_name,"event",5)!=0)continue;
  std::string dev="/dev/input/"+std::string(e->d_name);
  int f=open(dev.c_str(),O_RDONLY|O_NONBLOCK); if(f<0)continue;
  unsigned long ev_bits[(EV_MAX + 64) / 64]{};
  unsigned long key_bits[(KEY_MAX + 64) / 64]{};
  const bool has_rel = ioctl(f, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0 &&
                       (ev_bits[EV_REL/(sizeof(unsigned long)*8)] & (1UL << (EV_REL%(sizeof(unsigned long)*8))));
  const bool has_mouse_key = ioctl(f, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0 &&
                             ((key_bits[BTN_LEFT/(sizeof(unsigned long)*8)] & (1UL << (BTN_LEFT%(sizeof(unsigned long)*8)))) ||
                              (key_bits[BTN_RIGHT/(sizeof(unsigned long)*8)] & (1UL << (BTN_RIGHT%(sizeof(unsigned long)*8)))));
  close(f);
  if(has_rel && has_mouse_key){*out=dev;closedir(d);return true;}
 }
 closedir(d); return false;
#endif
}

// 建立 event.sock 连接并发送订阅请求，等待 ACK；任一步失败即关闭并返回 false
bool PhysicalMouseReader::open_event_socket(std::string* error) {
#if defined(_WIN32)
 (void)error; return false;
#else
 if(event_socket_path_.empty()) event_socket_path_=paths::kMouseEventSocketDefault;
 if(event_fd_>=0){::close(event_fd_);event_fd_=-1;}
 event_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
 if (event_fd_ < 0) { if(error)*error="创建 usb-proxy event socket 失败"; return false; }
 struct timeval tv{};
 tv.tv_sec = 0; tv.tv_usec = 500000;
 ::setsockopt(event_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 sockaddr_un addr{}; addr.sun_family=AF_UNIX;
 if(event_socket_path_.size() >= sizeof(addr.sun_path)) { if(error)*error="event socket 路径过长"; close(event_fd_); event_fd_=-1; return false; }
 std::memcpy(addr.sun_path,event_socket_path_.c_str(),event_socket_path_.size()+1);
 if(::connect(event_fd_,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0){if(error)*error="连接 usb-proxy event socket 失败: "+std::string(std::strerror(errno));close(event_fd_);event_fd_=-1;return false;}
 Header req{kMagic,kVersion,kSubscribeReq,1};
 if(::send(event_fd_,&req,sizeof(req),MSG_NOSIGNAL)!=static_cast<ssize_t>(sizeof(req))){if(error)*error="订阅 usb-proxy 按键事件失败";close(event_fd_);event_fd_=-1;return false;}
 unsigned char response[64]{}; const ssize_t n=::recv(event_fd_,response,sizeof(response),0);
 if(n<static_cast<ssize_t>(sizeof(Header))){if(error)*error="usb-proxy 按键订阅响应过短";close(event_fd_);event_fd_=-1;return false;}
 Header ack{};std::memcpy(&ack,response,sizeof(ack));
 if(ack.magic!=kMagic||ack.version!=kVersion||ack.type!=kSubscribeAck){if(error)*error="usb-proxy 按键订阅响应不匹配";close(event_fd_);event_fd_=-1;return false;}
 return true;
#endif
}

bool PhysicalMouseReader::start_event_socket(std::string* error) {
    // ★★ V1.0.27：socket 晚到时**重试**，别一次失败就永久放弃。
    //   现场事故（2026-10-04 升级 V1.0.25 后）：core 与 ttbox-usbproxy **同一秒**启动
    //   （systemd 无依赖关系），core 去 connect 时 usb-proxy 还没建 socket ⇒ ENOENT ⇒
    //   上层按「不阻塞 AI 流水线」放弃 PhysicalMouseReader ⇒ **物理鼠标输入链永久断开**
    //   （不会自动重连）⇒ 表现为"自瞄像没反应 / 报 event.sock fallback failed"。
    //   open_event_socket 每次失败都会 close 并复位 event_fd_ ⇒ 可安全重入。
    //   12 次 × 300ms ≈ 最长等 3.6s，足够覆盖 usb-proxy 的设备枚举时间。
    // ★ 用 std::this_thread::sleep_for 而不是 usleep/nanosleep —— 这段代码**不在**
    //   `#if defined(_WIN32)` 分支里（Windows 也要编过），usleep 在 MSVC 下不存在。
    constexpr int kConnectAttempts = 12;
    constexpr auto kRetryInterval = std::chrono::milliseconds(300);
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        if (open_event_socket(error)) {
            if (attempt > 1) {
                std::fprintf(stderr,
                             "PhysicalMouseReader: event.sock 第 %d 次尝试才连上"
                             "（usb-proxy 启动比 core 慢，已自动等待）\n", attempt);
            }
            std::fprintf(stderr, "PhysicalMouseReader: usb-proxy event.sock subscribed\n");
            event_thread_ = std::thread(&PhysicalMouseReader::event_socket_loop, this);
            return true;
        }
        std::this_thread::sleep_for(kRetryInterval);
    }
    return false;
}

// 启动读取：优先 evdev；device 为空或打开失败则回退 usb-proxy event.sock
bool PhysicalMouseReader::start(const std::string& requested,std::string* error){
#if defined(_WIN32)
 (void)requested; if(error)*error="Windows 不支持 evdev"; return false;
#else
 if(running_.exchange(true))return false;
 device_=requested;
 std::fprintf(stderr, "PhysicalMouseReader: start requested=%s\n", requested.c_str());
 if (device_.empty()) {
     std::fprintf(stderr, "PhysicalMouseReader: using usb-proxy event.sock\n");
     if(!start_event_socket(error)){std::fprintf(stderr, "PhysicalMouseReader: event.sock fallback failed: %s\n", error ? error->c_str() : "unknown");running_=false;return false;}
     return true;
 }
 fd_=open(device_.c_str(),O_RDONLY|O_NONBLOCK);
 if(fd_<0){
    const std::string evdev_error = std::strerror(errno);
    std::fprintf(stderr, "PhysicalMouseReader: evdev open failed (%s), using usb-proxy event.sock\n", evdev_error.c_str());
    if (start_event_socket(error)) return true;
    running_=false; if(error)*error="无法打开物理鼠标: "+device_+"；event.sock 回退失败"; return false;
 }
 thread_=std::thread(&PhysicalMouseReader::loop,this); return true;
#endif
}

// 停止读取：置停止标志、关闭 fd（唤醒阻塞读）、join 两个读取线程、清空按键位
void PhysicalMouseReader::stop(){
#if !defined(_WIN32)
 if(!running_.exchange(false))return;
 if(fd_>=0){close(fd_);fd_=-1;}
 if(event_fd_>=0){shutdown(event_fd_,SHUT_RDWR);close(event_fd_);event_fd_=-1;}
 if(thread_.joinable())thread_.join();
 if(event_thread_.joinable())event_thread_.join();
 buttons_.store(0,std::memory_order_release);
#endif
}

// evdev 读取线程主体：读 input_event，累计 REL 位移、按 EV_KEY 更新按键位图
void PhysicalMouseReader::loop(){
#if !defined(_WIN32)
 input_event ev{}; while(running_.load()){ if(read(fd_,&ev,sizeof(ev))!=(ssize_t)sizeof(ev)){usleep(1000);continue;} if(ev.type==EV_REL){if(ev.code==REL_X)rel_x_.fetch_add(ev.value);if(ev.code==REL_Y)rel_y_.fetch_add(ev.value);} if(ev.type==EV_KEY){uint16_t bit=0;if(ev.code==BTN_LEFT)bit=1;if(ev.code==BTN_RIGHT)bit=2;if(ev.code==BTN_MIDDLE)bit=4;if(ev.code==BTN_SIDE)bit=8;if(ev.code==BTN_EXTRA)bit=16;if(bit){if(ev.value)buttons_.fetch_or(bit);else buttons_.fetch_and((uint16_t)~bit);}} }
#endif
}

// event.sock 读取线程主体：收状态快照/按键事件更新按键位，连接陈旧则断开重连
void PhysicalMouseReader::event_socket_loop(){
#if !defined(_WIN32)
 unsigned char packet[128]{};
 auto last_reconnect_log = std::chrono::steady_clock::now() - std::chrono::seconds(1);
 while(running_.load(std::memory_order_acquire)){
   if(event_fd_<0){
     // usb-proxy 重启/退出后自动重连；断开瞬间先 fail-closed 清空热键位，
     // 防止旧按键状态把注入门永久打开。
     buttons_.store(0,std::memory_order_release);
     std::string err;
     if(!open_event_socket(&err)){
       const auto now = std::chrono::steady_clock::now();
       if (now - last_reconnect_log >= std::chrono::seconds(1)) {
         std::fprintf(stderr, "PhysicalMouseReader: event.sock reconnect failed, retry\n");
         last_reconnect_log = now;
       }
       std::this_thread::sleep_for(std::chrono::milliseconds(100));
       continue;
     }
     std::fprintf(stderr, "PhysicalMouseReader: usb-proxy event.sock reconnected\n");
   }
   const ssize_t n=::recv(event_fd_,packet,sizeof(packet),0);
   // 服务端每 100ms 推一次状态快照；500ms 内无任何数据说明连接已陈旧，
   // 主动断开重连，避免永久卡在失效 socket 上。
   if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
     if(!running_.load())break;
     buttons_.store(0,std::memory_order_release);
     ::shutdown(event_fd_,SHUT_RDWR);
     ::close(event_fd_);
     event_fd_=-1;
     std::this_thread::sleep_for(std::chrono::milliseconds(100));
     continue;
   }
   if(n<=0){
     if(!running_.load())break;
     buttons_.store(0,std::memory_order_release);
     ::shutdown(event_fd_,SHUT_RDWR);
     ::close(event_fd_);
     event_fd_=-1;
     std::this_thread::sleep_for(std::chrono::milliseconds(100));
     continue;
   }
   if(n<static_cast<ssize_t>(sizeof(Header)+9))continue;
   Header h{};std::memcpy(&h,packet,sizeof(h));
   if(h.magic!=kMagic||h.version!=kVersion)continue;
   if(h.type==kStateSnapshot){
      const unsigned char mask=packet[sizeof(Header)];
      buttons_.store(static_cast<uint16_t>(mask),std::memory_order_release);
      continue;
   }
   if(h.type!=kButtonEvent||n<static_cast<ssize_t>(sizeof(Header)+11))continue;
   const unsigned char button=packet[8]; const unsigned char pressed=packet[9]; const unsigned char mask=packet[10];
   (void)button;
   buttons_.store(static_cast<uint16_t>(mask),std::memory_order_release);
   if(!pressed && mask==0) buttons_.store(0,std::memory_order_release);
 }
#endif
}
}
