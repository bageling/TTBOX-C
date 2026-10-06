"""Model Plugin 进程入口占位：生产模型推理仍由 Core ModelAdapter/RKNNEngine 承担。"""
import os
import sys
from pathlib import Path


def _ttbox_tree_root() -> Path:
    """定位 TTBOX 树根（开发机 = 仓库根；板端 = release 树根）。

    判据与 ``plugins/web/lib/paths.py::_ROOT_ANCHORS`` 同源：向上找**最近一层**
    同时含 plugins / framework / scripts / deploy 的目录。**不假设目录深度**。
    """
    cur = Path(__file__).resolve().parent
    while True:
        if all((cur / _n).is_dir() for _n in ("plugins", "usbproxy", "scripts", "deploy")):
            return cur
        if cur.parent == cur:
            raise RuntimeError(
                "找不到 TTBOX 树根：从 %s 向上未发现同时含 "
                "plugins/usbproxy/scripts/deploy 的目录" % __file__
            )
        cur = cur.parent


_TREE_ROOT = _ttbox_tree_root()
# 两处都 append 到 sys.path 末尾（不用 insert(0)：顶到 stdlib 前有遮蔽同名标准库的风险）
#   · <树根>/plugins/model —— 本插件目录（model_service）
#   · <树根>/plugins/web   —— lib.paths 路径单点真源（A-PATH-5）
sys.path.append(str(_TREE_ROOT / "plugins" / "model"))
sys.path.append(str(_TREE_ROOT / "plugins" / "web"))

from lib.paths import models_root  # noqa: E402
from model_service import ModelPluginService  # noqa: E402


def main():
    # 模型库根单点真源（V-04）：与 Core / web 同根，不散写 "/opt/ttbox/models"。
    root = Path(os.environ.get("TTBOX_MODELS_ROOT", models_root()))
    service = ModelPluginService(root)
    print(f"TTBOX Model Plugin ready: {root}", flush=True)
    import signal
    signal.pause()


if __name__ == "__main__":
    main()
