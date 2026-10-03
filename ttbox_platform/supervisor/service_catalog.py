"""AIBOX 服务编排契约：声明依赖和恢复策略，不在 Web 中散落 systemctl。"""
from dataclasses import dataclass
@dataclass(frozen=True)
class ServiceSpec:
 name:str; command:str; after:tuple[str,...]=(); restart:str='on-failure'; restart_sec:float=5.0; enabled:bool=True
AIBOX_ALIGNED_SERVICES=(
 ServiceSpec('ttbox-core','existing-core-entry',('network.target',),'always',5.0),
 ServiceSpec('ttbox-hid','existing-hid-entry',('ttbox-core',),'on-failure',5.0),
 ServiceSpec('ttbox-web','platform-web-entry',('network-online.target',),'always',1.0),
 ServiceSpec('ttbox-supervisor','platform-supervisor-entry',('ttbox-core','ttbox-web'),'on-failure',2.0),
)
class ServiceCatalog:
 def __init__(self,specs=AIBOX_ALIGNED_SERVICES): self._specs={x.name:x for x in specs}
 def get(self,name): return self._specs[name]
 def names(self): return tuple(self._specs)
