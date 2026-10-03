"""插件生命周期运行时与健康检查。"""
from __future__ import annotations
import subprocess
import urllib.request
from pathlib import Path
from typing import Callable
from .models import PluginHealth

class PluginRuntime:
    def start(self): raise NotImplementedError
    def stop(self): raise NotImplementedError
    def is_running(self): return False
    def health(self): return PluginHealth.UNKNOWN

class BuiltinPluginRuntime(PluginRuntime):
    def __init__(self, start: Callable | None = None, stop: Callable | None = None): self._start=start; self._stop=stop; self.running=False
    def start(self):
        if self._start: self._start()
        self.running=True
    def stop(self):
        if self._stop: self._stop()
        self.running=False
    def is_running(self): return self.running
    def health(self): return PluginHealth.HEALTHY if self.running else PluginHealth.UNKNOWN

class ProcessPluginRuntime(PluginRuntime):
    def __init__(self, entry: str | Path, cwd: str | Path | None = None, popen_factory=subprocess.Popen, stop_timeout: float = 10.0):
        self.entry=Path(entry); self.cwd=cwd; self._popen_factory=popen_factory; self.process=None
        # manifest 的 stop_timeout（standard.py 校验 1–3600）此前被硬编码 10 静默覆盖
        self._stop_timeout = stop_timeout
    def start(self):
        if self.process is None or self.process.poll() is not None:
            self.process=self._popen_factory([str(self.entry)], cwd=str(self.cwd) if self.cwd else None)
    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=self._stop_timeout)
            except subprocess.TimeoutExpired:
                # ★ 忽略 SIGTERM 的子进程不能就此泄漏：升级 SIGKILL（对照
                # ttbox_platform/runtime/process_adapter.py 的做法），否则进程带 fd/端口/子线程
                # 残留，同一插件再 start 会双实例。
                self.process.kill()
                self.process.wait(timeout=10)
    def is_running(self): return self.process is not None and self.process.poll() is None
    def health(self): return PluginHealth.HEALTHY if self.is_running() else PluginHealth.FAILED

class HealthChecker:
    def check(self, record, runtime: PluginRuntime | None = None) -> PluginHealth:
        definition = getattr(record, "health_check", None)
        if runtime is not None and not runtime.is_running(): return PluginHealth.FAILED
        if not definition: return runtime.health() if runtime else PluginHealth.UNKNOWN
        kind = definition.get("kind", "process")
        try:
            if kind == "process": return PluginHealth.HEALTHY if runtime and runtime.is_running() else PluginHealth.FAILED
            if kind == "command":
                result = subprocess.run(definition["command"], shell=False, timeout=definition.get("timeout_ms", 1000)/1000, check=False)
                return PluginHealth.HEALTHY if result.returncode == 0 else PluginHealth.FAILED
            if kind == "http":
                with urllib.request.urlopen(definition["url"], timeout=definition.get("timeout_ms", 1000)/1000) as response:
                    return PluginHealth.HEALTHY if 200 <= response.status < 300 else PluginHealth.DEGRADED
            return PluginHealth.UNKNOWN
        except Exception: return PluginHealth.FAILED
