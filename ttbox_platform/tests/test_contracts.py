import unittest
from ttbox_platform.runtime.lifecycle import RuntimeState, transition
from ttbox_platform.config.layers import ConfigLayer
from ttbox_platform.update.components import Component, UpdateStage
class ContractTests(unittest.TestCase):
 def test_runtime_happy_path(self):
  s=RuntimeState.STOPPED
  for t in (RuntimeState.STARTING,RuntimeState.READY,RuntimeState.RUNNING,RuntimeState.STOPPING,RuntimeState.STOPPED): s=transition(s,t)
  self.assertEqual(s,RuntimeState.STOPPED)
 def test_runtime_rejects_skip(self):
  with self.assertRaises(ValueError): transition(RuntimeState.STOPPED,RuntimeState.RUNNING)
 def test_contract_enums(self):
  self.assertEqual(ConfigLayer.OVERRIDE.value,"override")
  self.assertEqual(Component.MODEL.value,"model")
  self.assertEqual(UpdateStage.ROLLED_BACK.value,"rolled_back")
if __name__=="__main__": unittest.main()
