"""独立组件更新契约；失败时保留 current 与旧版本，避免设备不可启动。"""
from enum import Enum
class Component(str, Enum):
    CORE="core"; MODEL="model"; WEB="web"; CONFIG="config"; SUPERVISOR="supervisor"
class UpdateStage(str, Enum):
    STAGING="staging"; VALIDATED="validated"; INSTALLED="installed"; ACTIVE="active"; ROLLED_BACK="rolled_back"; FAILED="failed"
