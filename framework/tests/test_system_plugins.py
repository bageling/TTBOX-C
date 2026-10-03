import json
import tempfile
import unittest
from pathlib import Path

from plugins.system_common import (
    FanService, NetworkService, SystemService, MonitorService, LogService, UpgradeService, WifiService,
)
from plugins.system_host import SystemPluginHost

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
ROOT = Path(_ttbox_repo_root())


class SystemPluginTests(unittest.TestCase):
    def test_all_manifests_and_entries(self):
        for plugin_id in ("fan", "wifi", "network", "monitor", "system", "log", "upgrade"):
            manifest = json.loads((ROOT / "plugins" / plugin_id / "plugin.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["id"], plugin_id)
            self.assertEqual(manifest["api_version"], "1")
            self.assertIn(manifest["type"], ("process", "builtin"))
            self.assertTrue((ROOT / "plugins" / plugin_id / manifest["entry"]).is_file())

    def test_system_and_monitor_are_read_only_and_injectable(self):
        system = SystemService(command_runner=lambda argv: (0, "Linux test 1.0\n"))
        self.assertEqual(system.status()["kernel"], "Linux test 1.0")
        monitor = MonitorService(proc_root=Path(tempfile.mkdtemp()))
        self.assertIn("cpu", monitor.status())
        self.assertIn("memory", monitor.status())
        self.assertIn("disk", monitor.status())

    def test_network_and_wifi_use_argv_commands(self):
        calls = []
        def runner(argv):
            calls.append(argv)
            return 0, "wlan0:wifi:connected:TTBOX\n"
        wifi = WifiService(command_runner=runner)
        self.assertEqual(wifi.status()["nmcli"], True)
        wifi.scan()
        self.assertTrue(all(isinstance(argv, list) for argv in calls))
        network = NetworkService(command_runner=lambda argv: (0, "1.2.3.4\n"))
        self.assertEqual(network.status()["hostname"], network.status()["hostname"])

    def test_fan_log_and_upgrade_adapters_are_safe(self):
        fan = FanService(sys_root=Path(tempfile.mkdtemp()))
        self.assertFalse(fan.status()["available"])
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "ttbox.log"; log.write_text("one\ntwo\n", encoding="utf-8")
            self.assertEqual(LogService(log).read()["lines"], ["one", "two"])
        upgrade = UpgradeService(command_runner=lambda argv: (0, "ok"))
        self.assertEqual(upgrade.status()["state"], "unavailable")
        self.assertEqual(upgrade.check()["ok"], True)

    def test_host_aggregates_plugins_without_core_dependency(self):
        host = SystemPluginHost.create(log_path=Path(tempfile.mkdtemp()) / "ttbox.log", sys_root=Path(tempfile.mkdtemp()), proc_root=Path(tempfile.mkdtemp()))
        result = host.status()
        self.assertEqual(set(result), {"fan", "wifi", "network", "monitor", "system", "log", "upgrade"})


if __name__ == "__main__":
    unittest.main()
