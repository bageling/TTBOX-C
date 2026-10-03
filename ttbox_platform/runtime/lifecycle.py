"""TTBOX Platform Runtime 生命周期契约（平台控制面，不承载瞄准算法）。"""
from enum import Enum

class RuntimeState(str, Enum):
    STOPPED="STOPPED"; STARTING="STARTING"; READY="READY"; RUNNING="RUNNING"
    STOPPING="STOPPING"; FAILED="FAILED"; RECOVERING="RECOVERING"

_ALLOWED={
 RuntimeState.STOPPED:{RuntimeState.STARTING},
 RuntimeState.STARTING:{RuntimeState.READY,RuntimeState.FAILED},
 RuntimeState.READY:{RuntimeState.RUNNING,RuntimeState.STOPPING,RuntimeState.FAILED},
 RuntimeState.RUNNING:{RuntimeState.STOPPING,RuntimeState.FAILED},
 RuntimeState.STOPPING:{RuntimeState.STOPPED,RuntimeState.FAILED},
 RuntimeState.FAILED:{RuntimeState.RECOVERING,RuntimeState.STOPPED},
 RuntimeState.RECOVERING:{RuntimeState.STARTING,RuntimeState.FAILED},
}

def transition(current: RuntimeState, target: RuntimeState) -> RuntimeState:
    if target not in _ALLOWED.get(current,set()):
        raise ValueError(f"invalid runtime transition: {current.value} -> {target.value}")
    return target
