import importlib.util
import tempfile
import unittest
from pathlib import Path

from flask import Flask

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
ROOT = Path(_ttbox_repo_root())
WEB_DIR = ROOT / "plugins" / "web"


def load_framework_api():
    """按文件加载 framework_api.py 并独立构造 Flask app，验证其路由契约。

    ★ framework_api.py 现在**自带路径 bootstrap**（A-PATH-3 根锚发现），测试不再替它
      插 sys.path —— 原写法用 `sys.path.insert(0, ...)` 救活导入，等于把"它到底能不能
      在正常入口下导入"这件事藏进测试里（P6 收敛项）。
    """
    spec = importlib.util.spec_from_file_location(
        "web_framework_api_test", WEB_DIR / "framework_api.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    app = Flask("web_framework_api_test")
    module.install_framework_api(app)
    return module, app


class WebFrameworkApiTests(unittest.TestCase):
    def test_framework_and_market_api_are_registered(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            import os
            os.environ["TTBOX_PLUGINS_ROOT"] = str(root / "plugins")
            os.environ["TTBOX_PLUGIN_REPOSITORY_ROOT"] = str(root / "repository")
            module, app = load_framework_api()
            client = app.test_client()
            rules = {rule.rule for rule in app.url_map.iter_rules()}
            for route in ("/api/plugins", "/api/plugins/market", "/api/system/status",
                          "/api/core/status", "/api/model/list", "/api/model/active",
                          "/api/network/status", "/api/wifi/status", "/api/fan/status",
                          "/api/monitor/status", "/api/upgrade/status"):
                self.assertIn(route, rules)
            self.assertEqual(client.get("/api/plugins").status_code, 200)
            self.assertEqual(client.get("/api/plugins/market").status_code, 200)

    def test_market_api_only_reads_local_repository(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            import os
            os.environ["TTBOX_PLUGINS_ROOT"] = str(root / "plugins")
            os.environ["TTBOX_PLUGIN_REPOSITORY_ROOT"] = str(root / "repository")
            module, app = load_framework_api()
            client = app.test_client()
            response = client.get("/api/plugins/market", query_string={"q": "web"})
            payload = response.get_json()
            self.assertTrue(payload["ok"])
            self.assertEqual(payload["data"]["source"], "local")
            self.assertEqual(payload["data"]["online"], False)


if __name__ == "__main__":
    unittest.main()
