"""统一 Health：聚合 Runtime、systemd service 和平台基础检查。"""
from __future__ import annotations
from dataclasses import dataclass
import time
from typing import Any
from .checks import PlatformHealth
from ..supervisor.systemd_adapter import ServiceAdapter

@dataclass(frozen=True)
class UnifiedHealth:
    ok: bool; checks: dict[str,Any]; timestamp: float
    def as_dict(self): return {'ok':self.ok,'checks':self.checks,'timestamp':self.timestamp}

class HealthMonitor:
    def __init__(self, runtime, services: ServiceAdapter, service_names=(), model_root=None):
        self.runtime=runtime; self.services=services; self.service_names=tuple(service_names); self.base=PlatformHealth(runtime,model_root)
    def check(self):
        checks=self.base.check().checks
        for name in self.service_names:
            s=self.services.status(name); checks['service:'+name]={'ok':s.active,'active':s.active,'sub_state':s.sub_state,'pid':s.main_pid,'error':s.error}
        ok=all(bool(x.get('ok')) for x in checks.values())
        return UnifiedHealth(ok,checks,time.time())
