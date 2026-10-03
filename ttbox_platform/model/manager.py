"""AIBOX 对齐的模型生命周期控制面；不解析 RKNN tensor，也不改 Core。"""
from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import hashlib, json, os, shutil, tempfile

@dataclass(frozen=True)
class ModelInfo:
    model_id: str; state: str; path: str

class ModelManager:
    def __init__(self, root: str|Path):
        self.root=Path(root).resolve(); self.staging=self.root/'staging'; self.versions=self.root/'versions'; self.current=self.root/'current'; self.previous=self.root/'previous'
        for p in (self.staging,self.versions): p.mkdir(parents=True,exist_ok=True)
    def _id(self, model_id):
        if not model_id or Path(model_id).name != model_id or model_id in ('.','..') or any(c in model_id for c in '/\\'):
            raise ValueError('invalid model id')
        return model_id
    def _inside_versions(self, path: Path) -> Path:
        resolved=path.resolve()
        try: resolved.relative_to(self.versions.resolve())
        except ValueError as exc: raise ValueError('model path escapes versions directory') from exc
        return resolved
    def _version_dir(self, model_id): return self._inside_versions(self.versions/self._id(model_id))
    @staticmethod
    def _digest(path):
        h=hashlib.sha256();
        with path.open('rb') as f:
            for chunk in iter(lambda:f.read(1024*1024),b''): h.update(chunk)
        return h.hexdigest()
    def upload(self, model_id: str, source: str|Path) -> ModelInfo:
        mid=self._id(model_id); src=Path(source).resolve()
        if not src.is_file(): raise FileNotFoundError(src)
        dst=self.staging/mid; dst.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src,dst/'model.rknn'); marker=dst/'validation.json'
        if marker.exists(): marker.unlink()
        return ModelInfo(mid,'staging',str(dst))
    def validate(self, model_id: str) -> ModelInfo:
        mid=self._id(model_id); d=self.staging/mid; f=d/'model.rknn'
        if not f.is_file() or f.stat().st_size==0: raise ValueError('staged model is missing or empty')
        marker={'ok':True,'model_id':mid,'size':f.stat().st_size,'sha256':self._digest(f)}
        (d/'validation.json').write_text(json.dumps(marker,ensure_ascii=False),encoding='utf-8')
        return ModelInfo(mid,'validated',str(d))
    def install(self, model_id: str) -> ModelInfo:
        mid=self._id(model_id); src=self.staging/mid; marker=src/'validation.json'; f=src/'model.rknn'
        if not marker.is_file() or not f.is_file(): raise ValueError('model must be validated before install')
        meta=json.loads(marker.read_text(encoding='utf-8'))
        if meta.get('model_id')!=mid or meta.get('size')!=f.stat().st_size or meta.get('sha256')!=self._digest(f): raise ValueError('staged model changed after validation')
        dst=self.versions/mid
        temp=Path(tempfile.mkdtemp(prefix=f'.{mid}.',dir=self.versions))
        backup=None
        try:
            shutil.copytree(src,temp/'payload',dirs_exist_ok=True)
            if dst.exists() or dst.is_symlink():
                backup=self.versions/f'.{mid}.backup.{os.getpid()}'
                if backup.exists() or backup.is_symlink():
                    backup.unlink() if backup.is_symlink() or backup.is_file() else shutil.rmtree(backup)
                os.replace(dst,backup)
            os.replace(temp/'payload',dst)
            if backup is not None:
                shutil.rmtree(backup,ignore_errors=True)
        except Exception:
            if dst.exists() or dst.is_symlink():
                if dst.is_symlink() or dst.is_file(): dst.unlink()
                else: shutil.rmtree(dst,ignore_errors=True)
            if backup is not None and (backup.exists() or backup.is_symlink()): os.replace(backup,dst)
            raise
        finally:
            if temp.exists(): shutil.rmtree(temp,ignore_errors=True)
        return ModelInfo(mid,'installed',str(dst))
    def _atomic_activate(self, target: Path):
        temp=self.root/f'.current.tmp.{os.getpid()}'
        if temp.exists() or temp.is_symlink(): temp.unlink()
        temp.symlink_to(target,target_is_directory=True)
        try:
            os.replace(temp,self.current)
        except PermissionError:
            # Windows 对“替换已有目录符号链接”拒绝原子 replace；先移除链接再替换临时链接。
            # Linux/RK3588 仍优先走上面的原子路径。
            if self.current.is_symlink(): self.current.unlink()
            os.replace(temp,self.current)
        except Exception:
            if temp.exists() or temp.is_symlink(): temp.unlink()
            raise
    def activate(self, model_id: str) -> ModelInfo:
        mid=self._id(model_id); src=self._version_dir(mid)
        if not (src/'model.rknn').is_file(): raise ValueError('model is not installed')
        old_id=self.current.resolve().name if self.current.is_symlink() and self.current.exists() else None
        self._atomic_activate(src)
        if old_id and old_id != mid: self.previous.write_text(old_id,encoding='utf-8')
        elif old_id is None and self.previous.exists(): self.previous.unlink()
        return ModelInfo(mid,'active',str(src))
    def rollback(self) -> ModelInfo:
        if not self.current.is_symlink(): raise ValueError('no active model')
        if not self.previous.is_file(): raise ValueError('no previous model')
        prev_id=self._id(self.previous.read_text(encoding='utf-8').strip()); prev=self._version_dir(prev_id)
        if not (prev/'model.rknn').is_file(): raise ValueError('previous model is missing')
        current_id=self.current.resolve().name
        self._atomic_activate(prev); self.previous.write_text(current_id,encoding='utf-8')
        return ModelInfo(prev_id,'active',str(prev))
    def list(self) -> list[ModelInfo]:
        active=self.current.resolve() if self.current.is_symlink() and self.current.exists() else None
        return [ModelInfo(p.name,'active' if active and p.resolve()==active else 'installed',str(p)) for p in sorted(self.versions.iterdir()) if p.is_dir() and not p.is_symlink()]
