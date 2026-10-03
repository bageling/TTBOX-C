import tempfile, unittest
from pathlib import Path
from ttbox_platform.model.manager import ModelManager
class ModelManagerTests(unittest.TestCase):
 def test_upload_validate_install_activate_rollback(self):
  with tempfile.TemporaryDirectory() as td:
   root=Path(td)/'models'; src=Path(td)/'x.rknn'; src.write_bytes(b'fake-rknn')
   m=ModelManager(root); m.upload('v1',src); m.validate('v1'); m.install('v1'); m.activate('v1')
   self.assertEqual(m.current.resolve().name,'v1')
   src.write_bytes(b'v2'); m.upload('v2',src); m.validate('v2'); m.install('v2'); m.activate('v2'); self.assertEqual(m.current.resolve().name,'v2')
   self.assertEqual(m.rollback().model_id,'v1'); self.assertEqual(m.rollback().model_id,'v2')
 def test_upload_invalidates_old_validation(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td); src=p/'x'; src.write_bytes(b'one'); m=ModelManager(p/'m'); m.upload('v1',src); m.validate('v1'); src.write_bytes(b'two'); m.upload('v1',src)
   with self.assertRaises(ValueError): m.install('v1')
 def test_validation_is_required(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td); src=p/'x'; src.write_bytes(b'x'); m=ModelManager(p/'m'); m.upload('v1',src)
   with self.assertRaises(ValueError): m.install('v1')
 def test_rejects_traversal(self):
  with tempfile.TemporaryDirectory() as td:
   m=ModelManager(Path(td)/'m');
   with self.assertRaises(ValueError): m.upload('../bad',Path(td)/'x')
 def test_rejects_versions_symlink(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td); outside=p/'outside'; outside.mkdir(); (outside/'model.rknn').write_bytes(b'x'); m=ModelManager(p/'m'); (m.versions/'evil').symlink_to(outside,target_is_directory=True)
   with self.assertRaises(ValueError): m.activate('evil')
 def test_rejects_tampered_previous(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td); outside=p/'outside'; outside.mkdir(); m=ModelManager(p/'m'); m.previous.write_text(str(outside),encoding='utf-8')
   with self.assertRaises(ValueError): m.rollback()
if __name__=='__main__': unittest.main()
