import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
ROOT = Path(_ttbox_repo_root())
WEB_PLUGIN = ROOT / "plugins" / "web"


class WebPluginTests(unittest.TestCase):
    def test_manifest_and_layout(self):
        manifest = json.loads((WEB_PLUGIN / "plugin.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["id"], "web")
        self.assertEqual(manifest["type"], "process")
        self.assertEqual(manifest["api_version"], "1")
        self.assertTrue((WEB_PLUGIN / manifest["entry"]).is_file())
        self.assertTrue((WEB_PLUGIN / "templates" / "index.html").is_file())
        # E01：旧前端 2026-09-18 迁入 static/legacy/，2026-09-24 随面板收敛**删除**
        #   （740K 死资产，现役 index.html 自包含、零引用；归档在
        #   .workbuddy/artifacts/archive/legacy-static-2026-09-24/，也可用 git 历史回滚）。
        #   断言反转为「必须不存在」，防止哪天被误拷回来。
        self.assertFalse((WEB_PLUGIN / "static" / "legacy").exists())

    def test_flask_routes_and_pages_from_migrated_entry(self):
        entry = WEB_PLUGIN / "bin" / "ttbox-web.py"
        spec = importlib.util.spec_from_file_location("ttbox_web_plugin", entry)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        client = module.app.test_client()
        # 激活 gate（M2.07）：未激活 ⇒ 需激活页面 302 /activate（2026-09-18 实证 HEAD 亦如此，
        # 测试环境无云会话属预期）；激活页本体与 /mobile 恒 200。
        r_home = client.get("/")
        self.assertEqual(r_home.status_code, 302)
        self.assertTrue(r_home.headers.get("Location", "").endswith("/activate"))
        self.assertEqual(client.get("/activate").status_code, 200)
        # 旧前端资产已删（2026-09-24）⇒ 这条路径必须 404，不能还能被当静态资源取到
        self.assertEqual(client.get("/static/legacy/app.js").status_code, 404)
        rules = {rule.rule for rule in module.app.url_map.iter_rules()}
        # S1-2026-09-18：/api/hailo/status、/api/network/wifi 随页签移除已删，断言同步
        for route in ("/api/state", "/api/models", "/api/config", "/api/system", "/api/preview.mjpg"):
            self.assertIn(route, rules)
        for removed in ("/api/hailo/status", "/api/hailo/install", "/api/network/wifi",
                        "/api/system/master-reactivate", "/api/system/version",
                        "/api/health/frontend", "/api/settings/auto-start",
                        "/api/hardware/mouse/timing", "/api/models/game-profile",
                        "/api/models/cloud-encrypted", "/api/preview.jpg",
                        "/api/activation/network/prepare", "/api/mouse-output/test-circle",
                        "/api/makcu/devices", "/api/ferrum/devices", "/api/kmboxb/devices"):
            self.assertNotIn(removed, rules)

    def test_ttbox_state_and_license_contract(self):
        entry = WEB_PLUGIN / "bin" / "ttbox-web.py"
        spec = importlib.util.spec_from_file_location("ttbox_web_state_contract", entry)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module._get_status = lambda: {"running": True, "runtime_running": True}
        module._get_runtime_profile = lambda: {
            "model_id": "ttbox-model",
            "mouse": {"mode": "local_hid"},
        }
        module.ipc_request = lambda request_type, *_args: {
            "status": 0,
            "data": {"models": []},
        } if request_type == "MODEL_LIST" else {"status": 1, "data": {}}

        state = module.collect_web_state()["data"]
        license_data = module._license_payload()

        self.assertEqual(state["selected_model_id"], "ttbox-model")
        self.assertEqual(state["state"]["selected_model_id"], "ttbox-model")
        self.assertEqual(state["config"]["mouse_output"]["mode"], "full_passthrough")
        self.assertEqual(state["state"]["mouse_output"]["mode"], "full_passthrough")
        self.assertEqual(license_data["app_version"], module.kAppVersion)
        self.assertEqual(license_data["ui"]["brand_name"], "TTBOX")
        self.assertEqual(license_data["ui"]["ui_brand"], "ttbox")
        self.assertEqual(license_data["ui_brand"], "ttbox")
        self.assertEqual(module._CONVERTER_SCRIPT, "/opt/ttbox/tools/converter/convert_onnx_to_rknn.py")
        self.assertEqual(module._CONVERT_CALIB_DIR, "/opt/ttbox/calib/imgs")

    def test_plugin_manifest_is_accepted_by_manager(self):
        from framework.plugin_manager.standard import PluginPackage
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "web"
            import shutil
            shutil.copytree(WEB_PLUGIN, target)
            inspected = PluginPackage.inspect if False else None
            from framework.plugin_manager.standard import PluginManifest
            manifest = PluginManifest.from_dict(json.loads((target / "plugin.json").read_text(encoding="utf-8")))
            self.assertEqual(manifest.plugin_id, "web")


if __name__ == "__main__":
    unittest.main()
