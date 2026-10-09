// EP0 主循环（proxy.cpp 定义）：处理枚举/配置/接口切换与描述符转发。
void ep0_loop(int fd);
// 物理断连停机（原子守卫只发一次 SIGINT；proxy.cpp 定义）。
// hotplug 回调与端点线程的断连路径共用，防止双 SIGINT 命中强退分支。
void stop_proxy_after_physical_disconnect();
