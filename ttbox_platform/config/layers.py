"""统一配置层级名称，实际合并/校验由后续 ConfigService 实现。"""
from enum import Enum
class ConfigLayer(str, Enum):
    FACTORY="factory"; DEVICE="device"; RUNTIME="runtime"; OVERRIDE="override"
