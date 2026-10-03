"""Core 进程适配器：只负责启动/停止/探活现有 C++ Core，不实现任何推理链路。"""
from __future__ import annotations
from dataclasses import dataclass
import os, signal, subprocess, time
from typing import Optional, Sequence

@dataclass(frozen=True)
class ProcessInfo:
    pid: Optional[int] = None
    started_at: Optional[float] = None

class ProcessAdapter:
    def start(self) -> ProcessInfo: raise NotImplementedError
    def stop(self) -> None: raise NotImplementedError
    def is_running(self) -> bool: raise NotImplementedError
    def health(self) -> bool: return self.is_running()

class SubprocessProcessAdapter(ProcessAdapter):
    """通过现有 Core 可执行文件/启动脚本启动进程；不创建第二套 Runtime。"""
    def __init__(self, command: Sequence[str], *, cwd: str|None=None, env: dict|None=None):
        if not command: raise ValueError("core command must not be empty")
        self.command=list(command); self.cwd=cwd; self.env=env
        self._process: subprocess.Popen|None=None; self._started_at: float|None=None

    def start(self) -> ProcessInfo:
        if self.is_running(): return ProcessInfo(self._process.pid, self._started_at)
        self._process=subprocess.Popen(self.command,cwd=self.cwd,env=self.env,
                                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self._started_at=time.time(); return ProcessInfo(self._process.pid,self._started_at)
    def stop(self) -> None:
        if not self._process: return
        if self._process.poll() is None:
            self._process.terminate()
            try: self._process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._process.kill(); self._process.wait()
        self._process=None; self._started_at=None
    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None
    def health(self) -> bool: return self.is_running()

class MockProcessAdapter(ProcessAdapter):
    """测试适配器：模拟 Core 进程，不伪装成真实硬件集成。"""
    def __init__(self): self.running=False; self.pid: int|None=None; self.started_at: float|None=None; self.starts=0; self.stops=0
    def start(self) -> ProcessInfo:
        self.running=True; self.starts+=1; self.pid=10000+self.starts; self.started_at=time.time(); return ProcessInfo(self.pid,self.started_at)
    def stop(self) -> None: self.running=False; self.stops+=1; self.pid=None; self.started_at=None
    def is_running(self) -> bool: return self.running
