"""模型组件更新状态契约；不解析 RKNN tensor，不触碰 Core 算法。"""
from enum import Enum
class ModelState(str, Enum):
    STAGING="staging"; VALIDATED="validated"; INSTALLED="installed"; ACTIVE="active"; ROLLBACK="rollback"; FAILED="failed"
