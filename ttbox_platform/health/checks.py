"""平台健康检查最小实现；硬件检查由 RK3588 adapter 后续接入。"""
from dataclasses import dataclass
import os, shutil, time
@dataclass(frozen=True)
class HealthReport:
 ok:bool; checks:dict; timestamp:float
class PlatformHealth:
 def __init__(self,runtime=None,model_root=None): self.runtime=runtime; self.model_root=model_root
 def check(self):
  checks={}
  if self.runtime: checks['runtime']=self.runtime.health()
  checks['storage']={'ok':os.path.isdir(self.model_root) if self.model_root else True}
  checks['python']= {'ok':shutil.which('python') is not None or shutil.which('python3') is not None}
  ok=all(bool(v.get('ok')) for v in checks.values())
  return HealthReport(ok,checks,time.time())
