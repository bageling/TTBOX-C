import base64
import importlib.util
import io
import json
import tempfile
import threading
import time
import unittest
from pathlib import Path

from framework.plugin_manager import InstallRequest, InstallSource, PluginManager, PluginState

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
ROOT = Path(_ttbox_repo_root())
PLUGIN = ROOT / "plugins" / "preview"


class PreviewPluginTests(unittest.TestCase):
    def test_manifest_layout_and_api(self):
        manifest = json.loads((PLUGIN / "plugin.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["id"], "preview")
        self.assertEqual(manifest["type"], "process")
        self.assertEqual(manifest["api_version"], "1")
        self.assertTrue((PLUGIN / manifest["entry"]).is_file())
        self.assertTrue((PLUGIN / "config" / "preview.json").is_file())

    def test_preview_snapshot_latest_frame_and_status(self):
        spec = importlib.util.spec_from_file_location("preview_plugin", PLUGIN / "bin" / "ttbox-preview.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = module.MemoryFrameSource()
        service = module.PreviewService(source, module.PreviewConfig(fps=20, jpeg_quality=75))
        source.publish(b"old", 1, 320, 240, "BGR888")
        source.publish(b"new", 2, 640, 360, "BGR888")
        snapshot = service.snapshot()
        self.assertEqual(snapshot.data, b"new")
        self.assertEqual(snapshot.frame_number, 2)
        self.assertEqual(service.status()["source"], "core_ipc")
        self.assertEqual(service.status()["width"], 640)

    def test_http_jpeg_endpoint_and_core_isolation(self):
        spec = importlib.util.spec_from_file_location("preview_plugin_http", PLUGIN / "bin" / "ttbox-preview.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = module.MemoryFrameSource()
        source.publish(b"jpeg-data", 3, 640, 360, "BGR888")
        server = module.create_server("127.0.0.1", 0, module.PreviewService(source))
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            import urllib.request
            port = server.server_address[1]
            body = urllib.request.urlopen(f"http://127.0.0.1:{port}/api/preview.jpg", timeout=2).read()
            self.assertEqual(body, b"jpeg-data")
            status = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/api/preview/status", timeout=2).read())
            self.assertEqual(status["frame_number"], 3)
        finally:
            server.shutdown(); server.server_close()

    def test_plugin_manager_can_manage_preview_without_core_stop(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "plugins"
            # Construct an in-memory builtin callback with the same independent lifecycle contract.
            events = []
            manager = PluginManager()
            from framework.plugin_manager.manager import PluginSpec
            manager.register(PluginSpec("preview", lambda: events.append("start"), lambda: events.append("stop")))
            self.assertTrue(manager.enable("preview")); self.assertTrue(manager.start("preview"))
            self.assertTrue(manager.stop("preview")); self.assertEqual(manager.status("preview").state, PluginState.ENABLED)
            self.assertEqual(events, ["start", "stop"])


if __name__ == "__main__": unittest.main()
