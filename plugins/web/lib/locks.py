"""跨模块共享的运行时锁（2026-10-02 web 换写法 S4：单点化）。

_CFG_WRITE_LOCK —— 配置读-改-写串行锁。
Core 只保证单次 SET_CONFIG 原子，**不保证跨请求的 RMW 原子**：waitress 64 线程 +
标定线程并发时 A读→B读→A写→B写，A 的修改会被 B 的整份快照静默抹掉
（现象是"设置偶发不生效 / 被改回去"）。所有 GET_CONFIG→SET_CONFIG 序列必须持锁。

★ 单点定义：本模块。入口 ttbox-web.py 与 lib/ 各模块一律 import 这**一个**对象 ——
  各自 new 一把锁等于没加锁（2026-10-02 之前入口里就重复定义过两次，靠后一次覆盖
  前一次才侥幸没出错）。
★ 消费点必须写成**裸名** `with _CFG_WRITE_LOCK:`，不得改成 `hub.get('_CFG_WRITE_LOCK')`：
  test_web_calibration_apply.py 用 exec + 命名空间注入真锁的方式跑 _calib_apply_gain，
  改名后它注入的锁就落不到实处。

config_write_serialized —— 配置写串行化装饰器（2026-10-02 S9-b 从入口搬来）。
★ 为什么定义在这里：这是**无参装饰器**，在被装饰函数的 def 行执行时
  （也就是 **import 期**）就被应用。那一刻入口的 `hub.bind()` 还没跑，
  走 hub 转发必然 RuntimeError: hub 未绑定。
  它只依赖本模块的 _CFG_WRITE_LOCK ⇒ 搬到这里后 api/*.py 可直接 import。
  同样地，**任何装饰器都不能经 hub 转发** —— 装饰器求值时机永远早于 bind。
"""
import functools
import threading

_CFG_WRITE_LOCK = threading.RLock()


def config_write_serialized(fn):
    """把整个请求处理函数包进 _CFG_WRITE_LOCK（用于短平快的配置 RMW 端点）。

    长任务（标定 worker）不能整函数持锁，只包各 RMW 段。
    """
    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        with _CFG_WRITE_LOCK:
            return fn(*args, **kwargs)
    wrapper.__name__ = fn.__name__
    return wrapper
