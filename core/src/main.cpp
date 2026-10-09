// main.cpp — TTBox C++ Core 入口
//
// 生命周期：初始化 Application → run（事件循环）→ 正常退出。
// 支持 SIGINT / SIGTERM 优雅退出。
/*
 * TTBOX 文件说明
 *
 * 文件：main.cpp
 *
 * 作用：
 *   TTBOX 程序的入口点。
 *   操作系统启动程序后，首先执行的就是这个文件。
 *
 * 小白理解：
 *   就像按电脑电源键开机一样，这里是程序启动的大门。
 *   它负责：注册退出信号 → 创建 Application 对象 → 初始化 → 运行 → 退出。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include <csignal>
#include <cstdio>

#include "app/Application.hpp"
#include "common/Logger.hpp"

namespace {

void signal_handler(int) {
    // async-signal-safe：仅置原子标志，由 Application::run() 轮询退出
    ttbox::core::Application::request_shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ttbox::core::Application app;
    int rc = app.initialize(argc, argv);
    if (rc != 0) {
        return rc;
    }

    app.run();
    app.shutdown();
    // §5.2：WARN/INFO 走内存缓冲批量落盘 —— 进程退出前必须强制刷一次，
    // 否则最后不足 5 分钟的那批日志会随进程一起丢掉（现场排查最需要的就是最后那几行）。
    ttbox::core::Logger::instance().flush();
    return 0;
}
