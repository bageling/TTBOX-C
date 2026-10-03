import sys, time, unittest
from ttbox_platform.runtime.process_adapter import SubprocessProcessAdapter

class SubprocessAdapterTests(unittest.TestCase):
 def test_real_subprocess_lifecycle(self):
  a=SubprocessProcessAdapter([sys.executable,"-c","import time; time.sleep(30)"])
  info=a.start()
  try:
   self.assertTrue(a.is_running()); self.assertIsNotNone(info.pid); self.assertTrue(a.health())
  finally:
   a.stop()
  self.assertFalse(a.is_running())
if __name__=="__main__": unittest.main()
