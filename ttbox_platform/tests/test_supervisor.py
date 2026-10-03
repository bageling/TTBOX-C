import unittest
from ttbox_platform.runtime.controller import RuntimeController
from ttbox_platform.runtime.process_adapter import MockProcessAdapter, ProcessInfo
from ttbox_platform.supervisor.systemd_adapter import MockServiceAdapter
from ttbox_platform.supervisor.supervisor import Supervisor
from ttbox_platform.health.monitor import HealthMonitor
class SupervisorTests(unittest.TestCase):
 def setUp(self):
  self.svc=MockServiceAdapter(('ttbox-core','ttbox-hid'))
  class SyncProcess(MockProcessAdapter):
   def start(inner):
    info=super(SyncProcess,inner).start(); self.svc.states['ttbox-core']=True; return info
   def stop(inner):
    super(SyncProcess,inner).stop(); self.svc.states['ttbox-core']=False
  self.r=RuntimeController(SyncProcess()); self.h=HealthMonitor(self.r,self.svc,('ttbox-core','ttbox-hid')); self.s=Supervisor(self.r,self.svc,self.h,'ttbox-core',('ttbox-hid',))
 def test_start_status_health(self):
  x=self.s.start(); self.assertEqual(x.runtime.state,'RUNNING'); self.assertTrue(x.health.ok); self.assertTrue(x.services['ttbox-core'].active)
 def test_stop(self): self.s.start(); x=self.s.stop(); self.assertEqual(x.runtime.state,'STOPPED'); self.assertFalse(x.services['ttbox-core'].active)
 def test_restart(self): self.s.start(); x=self.s.restart(); self.assertEqual(x.runtime.state,'RUNNING'); self.assertIn(('stop','ttbox-hid'),self.svc.calls)
 def test_recovery(self): self.s.start(); self.r._state=__import__('ttbox_platform.runtime.lifecycle',fromlist=['RuntimeState']).RuntimeState.FAILED; x=self.s.recover(); self.assertTrue(x.recovered); self.assertEqual(x.runtime.state,'RUNNING')
 def test_health_failure(self): x=self.s.status(); self.assertFalse(x.health.ok)
if __name__=='__main__': unittest.main()
