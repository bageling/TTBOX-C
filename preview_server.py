"""本地预览启动器：用 Flask 内置 dev server 渲染完整 index.html（含 glass-modern.css）。
本机预览专用，不改动 ttbox-web.py 的 waitress 生产入口。

**本文件只在仓库树里用**（开发预览），不进出货清单（见 deploy/pack_manifest.txt）。
"""
import sys
from pathlib import Path

# 本文件就放在 TTBOX 树根 ⇒ 自身目录即树根（不是"往上跳 N 层"的深度假设）。
TREE_ROOT = Path(__file__).resolve().parent
WEB_BIN = TREE_ROOT / "plugins" / "web" / "bin" / "ttbox-web.py"
if not WEB_BIN.is_file():
    raise SystemExit("找不到 %s —— 本启动器必须放在 TTBOX 树根" % WEB_BIN)
# append 到 sys.path 末尾，不用 insert(0)：顶到 stdlib 前会遮蔽同名标准库模块
# （完整故障记录见 plugins/web/bin/ttbox-web.py 头部 platform 冲突说明）。
sys.path.append(str(WEB_BIN.parent))

# 设置必要的环境变量，让模板/静态目录正确解析
import os
os.environ.setdefault("TTBOX_WEB_PREVIEW", "1")

import importlib.util
spec = importlib.util.spec_from_file_location("ttbox_web_preview", str(WEB_BIN))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

app = mod.app
# 禁用 waitress 依赖：直接 run
if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8899, debug=False, threaded=True)
