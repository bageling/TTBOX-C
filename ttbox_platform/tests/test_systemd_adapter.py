import unittest
from types import SimpleNamespace
from ttbox_platform.supervisor.systemd_adapter import SystemdServiceAdapter
class SystemdAdapterTests(unittest.TestCase):
 def test_status_parsing_and_argv(self):
  calls=[]
  def run(argv,**kw):
   calls.append((argv,kw)); return SimpleNamespace(returncode=0,stdout="LoadState=loaded\nActiveState=active\nSubState=running\nMainPID=42\nResult=success\n",stderr="")
  s=SystemdServiceAdapter(runner=run).status("ttbox-core")
  self.assertTrue(s.active); self.assertEqual(s.main_pid,42); self.assertEqual(calls[0][0],['systemctl','show','ttbox-core','--no-page','--property=LoadState,ActiveState,SubState,MainPID,Result']); self.assertNotIn('shell',calls[0][1])
if __name__=='__main__': unittest.main()


class PrefixTests(unittest.TestCase):
 def test_sudo_prefix_uses_argv(self):
  from types import SimpleNamespace
  calls=[]
  def run(argv,**kw): calls.append(argv); return SimpleNamespace(returncode=0,stdout="ActiveState=active\nSubState=running\nMainPID=1\nLoadState=loaded\n",stderr="")
  SystemdServiceAdapter(['sudo','-n','systemctl'],run).status('x'); self.assertEqual(calls[0][0:3],['sudo','-n','systemctl'])
