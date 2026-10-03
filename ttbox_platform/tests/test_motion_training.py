import json
import time

import pytest

from ttbox_motion.training import (
    MotionProfileStore,
    MotionSampleError,
    MotionTrainingError,
    validate_motion_sample,
)


def sample(mode="reaction", inside=True):
    return {
        "schema": "ttbox.motion-sample.v1",
        "mode": mode,
        "completion": "dwell",
        "canvas": {"width": 640, "height": 480},
        "start": {"x": 10, "y": 10},
        "target": {"x": 100, "y": 100},
        "radius": 20,
        "browser": {"pointer_lock": True, "raw_update": True, "coalesced_events": False},
        "points": [
            {"dt": 10, "dx": 40, "dy": 40},
            {"dt": 10, "dx": 50 if inside else 1, "dy": 50 if inside else 1},
        ],
    }


def test_validate_motion_sample_accepts_ttbox_sample():
    result = validate_motion_sample(sample())
    assert result["mode"] == "reaction"
    assert result["point_count"] == 2


def test_validate_motion_sample_rejects_external_schema():
    # "外部 schema" 指**不是本仓契约值**的 schema。本用例原先把它赋成了本仓合法值
    # （ttbox.motion-sample.v1）却断言必须被拒 —— 自相矛盾，故永远红。
    payload = sample()
    payload["schema"] = "external.motion-sample.v1"
    with pytest.raises(MotionSampleError, match="schema"):
        validate_motion_sample(payload)


def test_validate_motion_sample_rejects_path_that_does_not_reach_target():
    with pytest.raises(MotionSampleError, match="target"):
        validate_motion_sample(sample(inside=False))


def test_store_creates_profile_and_training_lease(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    assert profile["id"] == "profile-1"
    session = store.start_session(profile["id"], now=100.0)
    assert session["profile_id"] == "profile-1"
    assert session["lease_expires_at"] == 130.0


def test_store_rejects_second_active_lease(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    store.start_session(profile["id"], now=100.0)
    with pytest.raises(MotionTrainingError, match="active"):
        store.start_session(profile["id"], now=101.0)


def test_store_persists_sample_and_statistics(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    session = store.start_session(profile["id"], now=100.0)
    updated = store.append_sample(session["id"], sample(), now=101.0)
    assert updated["sample_count"] == 1
    saved = json.loads((tmp_path / "profile-1" / "profile.json").read_text())
    assert saved["samples"][0]["schema"] == "ttbox.motion-sample.v1"
    assert saved["statistics"]["reaction_mean_ms"] == 20.0


def test_train_requires_both_modes_and_generates_deterministic_model(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    session = store.start_session(profile["id"], now=100.0)
    store.append_sample(session["id"], sample("reaction"), now=101.0)
    with pytest.raises(MotionTrainingError, match="both modes"):
        store.train(profile["id"])
    store.stop_session(session["id"], now=102.0)
    session = store.start_session(profile["id"], now=103.0)
    for index in range(6):
        store.append_sample(session["id"], sample("reaction"), now=104.0 + index)
    for index in range(6):
        store.append_sample(session["id"], sample("continuous"), now=110.0 + index)
    trained = store.train(profile["id"])
    assert trained["model"]["schema"] == "ttbox.motion-model.v1"
    assert len(trained["model"]["knots"]) == 32
    assert trained["model"]["ready"] is True
    assert store.train(profile["id"])["model"] == trained["model"]


def test_activate_requires_ready_model_and_writes_runtime_state(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    with pytest.raises(MotionTrainingError, match="ready"):
        store.activate(profile["id"])


def test_profile_can_be_renamed_and_deleted(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("旧名称")
    renamed = store.rename(profile["id"], "新名称")
    assert renamed["name"] == "新名称"
    assert (tmp_path / profile["id"] / "profile.json").exists()
    assert store.delete(profile["id"])["deleted"] is True
    # 读不存在档案时的错误文案由 _read() 统一给出："failed to open <path>"
    # （ttbox_motion/training.py:133 的注释表明这是有意为之，与 Web 侧 /api/presets/load 的
    #  "failed to open {pf}" 同口径）⇒ 断言对这句，而不是 "exist"。
    with pytest.raises(MotionTrainingError, match="failed to open"):
        store.list_profile(profile["id"])


def test_clear_samples_resets_model_and_statistics(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    session = store.start_session(profile["id"], now=100.0)
    store.append_sample(session["id"], sample(), now=101.0)
    cleared = store.clear_samples(profile["id"])
    assert cleared["sample_count"] == 0
    assert cleared["model"]["ready"] is False


def test_deactivate_clears_active_profile(tmp_path):
    store = MotionProfileStore(tmp_path)
    profile = store.create_profile("我的曲线")
    profile_path = tmp_path / profile["id"] / "profile.json"
    data = json.loads(profile_path.read_text())
    data["model"] = {"schema": "ttbox.motion-model.v1", "ready": True, "quality": 80, "knots": [0.5] * 32}
    profile_path.write_text(json.dumps(data))
    store.activate(profile["id"])
    assert store.list_profiles()["enabled"] is True
    assert store.list_profiles()["profiles"][0]["id"] == profile["id"]
    assert store.list_profiles()["profiles"][0]["model"]["exists"] is True
    assert store.deactivate()["enabled"] is False
