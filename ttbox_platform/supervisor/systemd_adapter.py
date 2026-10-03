"""systemd 适配器：Platform 只编排现有服务，不重实现 Core/HID/Web。"""
from __future__ import annotations
from dataclasses import dataclass
import subprocess
from typing import Sequence

@dataclass(frozen=True)
class ServiceStatus:
    name: str; active: bool; sub_state: str; main_pid: int|None; unit_state: str; error: str|None=None

class ServiceAdapter:
    def status(self, name: str) -> ServiceStatus: raise NotImplementedError
    def start(self, name: str) -> ServiceStatus: raise NotImplementedError
    def stop(self, name: str) -> ServiceStatus: raise NotImplementedError
    def restart(self, name: str) -> ServiceStatus: raise NotImplementedError

class SystemdServiceAdapter(ServiceAdapter):
    """使用 systemctl 的 argv 调用，拒绝 shell 拼接；Windows 只能做命令契约测试。"""
    def __init__(self, systemctl: str|Sequence[str]='systemctl', runner=subprocess.run): self.systemctl=([systemctl] if isinstance(systemctl,str) else list(systemctl)); self.runner=runner
    def _run(self, *args: str):
        return self.runner([*self.systemctl,*args],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=15)
    def status(self,name):
        r=self._run('show',name,'--no-page','--property=LoadState,ActiveState,SubState,MainPID,Result')
        if r.returncode not in (0,3): return ServiceStatus(name,False,'unknown',None,'not-found',r.stderr.strip() or r.stdout.strip())
        data={}
        for line in r.stdout.splitlines():
            if '=' in line:
                k,v=line.split('=',1); data[k]=v
        try: pid=int(data.get('MainPID','0')) or None
        except ValueError: pid=None
        return ServiceStatus(name,data.get('ActiveState')=='active',data.get('SubState','unknown'),pid,data.get('LoadState','unknown'),None if r.returncode==0 else data.get('Result'))
    def _action(self,action,name):
        r=self._run(action,name)
        if r.returncode!=0: return ServiceStatus(name,False,'unknown',None,'error',r.stderr.strip() or r.stdout.strip())
        return self.status(name)
    def start(self,name): return self._action('start',name)
    def stop(self,name): return self._action('stop',name)
    def restart(self,name): return self._action('restart',name)

class MockServiceAdapter(ServiceAdapter):
    """测试用 systemd 替身，不代表设备 service 已启动。"""
    def __init__(self,names=()): self.states={n:False for n in names}; self.calls=[]
    def status(self,n): return ServiceStatus(n,self.states.get(n,False),'running' if self.states.get(n,False) else 'dead',1234 if self.states.get(n,False) else None,'loaded')
    def start(self,n): self.calls.append(('start',n)); self.states[n]=True; return self.status(n)
    def stop(self,n): self.calls.append(('stop',n)); self.states[n]=False; return self.status(n)
    def restart(self,n): self.calls.append(('restart',n)); self.states[n]=True; return self.status(n)


class SystemdProcessAdapter:
    """将既有 systemd Core unit 适配为 RuntimeController 进程边界。"""
    def __init__(self, services: SystemdServiceAdapter, unit: str): self.services=services; self.unit=unit; self._started_at=None
    def start(self):
        import time
        status=self.services.start(self.unit)
        if not status.active: raise RuntimeError(status.error or f"failed to start {self.unit}")
        self._started_at=time.time()
        from ..runtime.process_adapter import ProcessInfo
        return ProcessInfo(status.main_pid,self._started_at)
    def stop(self): self.services.stop(self.unit); self._started_at=None
    def status(self): return self.services.status(self.unit)
    def is_running(self): return self.services.status(self.unit).active
    def health(self): return self.is_running()
