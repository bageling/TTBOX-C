import unittest
from ttbox_platform.runtime.lifecycle import RuntimeState, transition
from ttbox_platform.runtime.controller import RuntimeController
from ttbox_platform.runtime.process_adapter import MockProcessAdapter

class RuntimeControllerTests(unittest.TestCase):
 def setUp(self): self.adapter=MockProcessAdapter(); self.c=RuntimeController(self.adapter)
 def test_initial_stopped(self): self.assertEqual(self.c.status().state,"STOPPED")
 def test_transition_happy_path(self):
  s=RuntimeState.STOPPED
  for t in (RuntimeState.STARTING,RuntimeState.READY,RuntimeState.RUNNING,RuntimeState.STOPPING,RuntimeState.STOPPED): s=transition(s,t)
  self.assertEqual(s,RuntimeState.STOPPED)
 def test_start_and_stop(self):
  self.assertEqual(self.c.start().state,"RUNNING"); self.assertIsNotNone(self.c.status().pid)
  self.assertEqual(self.c.stop().state,"STOPPED")
 def test_illegal_transition(self):
  with self.assertRaises(ValueError): transition(RuntimeState.STOPPED,RuntimeState.RUNNING)
 def test_failed_recovery_path(self):
  self.c._state=RuntimeState.FAILED
  self.assertEqual(self.c.start().state,"RUNNING")
 def test_restart(self):
  self.c.start(); snap=self.c.restart(); self.assertEqual(snap.state,"RUNNING"); self.assertEqual(self.adapter.starts,2); self.assertEqual(self.adapter.stops,1)
 def test_reload(self):
  self.c.start(); snap=self.c.reload(); self.assertEqual(snap.state,"RUNNING"); self.assertEqual(self.adapter.starts,2)
 def test_health(self):
  self.assertFalse(self.c.health()["ok"]); self.c.start(); self.assertTrue(self.c.health()["ok"])
 def test_failed_health_stops_started_process(self):
  class Unhealthy(MockProcessAdapter):
   def health(self): return False
  a=Unhealthy(); c=RuntimeController(a); snap=c.start()
  self.assertEqual(snap.state,"FAILED"); self.assertFalse(a.running); self.assertIsNone(snap.pid)
 def test_external_failure_becomes_failed(self):
  self.c.start(); self.c.process.running=False; snap=self.c.mark_failed("core crashed"); self.assertEqual(snap.state,"FAILED"); self.assertEqual(snap.last_error,"core crashed")
 def test_health_exception_becomes_failed(self):
  class Broken(MockProcessAdapter):
   def health(self): raise RuntimeError("probe failed")
  a=Broken(); c=RuntimeController(a); c.start(); self.assertEqual(c.health()["ok"],False); self.assertEqual(c.state,RuntimeState.FAILED)
 def test_status_shape(self):
  d=self.c.status().as_dict(); self.assertEqual(set(d),{"state","pid","uptime","last_error","health","timestamp"})
 def test_uptime(self):
  self.c.start(); self.assertGreaterEqual(self.c.status().uptime,0.0)
if __name__=="__main__": unittest.main()
