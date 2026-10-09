from __future__ import annotations

import argparse
import ast
import hashlib
import json
import math
import re
import shutil
import sys
import tempfile
from pathlib import Path

import onnx
from onnx import TensorProto, helper, shape_inference


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
HEAD_CONV_RE = re.compile(
    r"^(?P<head_prefix>/model\.\d+)/(?:one2one_)?cv(?P<branch>[23])\.(?P<scale>\d+)/"
    r"(?:one2one_)?cv(?P=branch)\.(?P=scale)\.2/Conv$"
)
RAW9_AUX_RE = re.compile(r"^(?P<head_prefix>/model\.\d+)/raw9_aux\.(?P<scale>\d+)/ReduceSum$")
YOLOV5_DETECT_CONV_RE = re.compile(r"^(?P<head_prefix>/model\.\d+)/m\.(?P<scale>\d+)/Conv$")
MODEL_PREFIX_RE = re.compile(r"^(?P<prefix>/model\.\d+)(?:/|$)")
SAFE_PATH_TOKEN_RE = re.compile(r"[^A-Za-z0-9._-]+")
RAW_OUTPUT_KEEPALIVE_SCALE = 1.0e-4


def to_linux_path(path_str: str) -> str:
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", path_str)
    if not match:
        return path_str.replace("\\", "/")
    drive = match.group(1).lower()
    rest = match.group(2).replace("\\", "/")
    return f"/mnt/{drive}/{rest}"


def runtime_path(path_str: str) -> Path:
    return Path(to_linux_path(path_str)).expanduser()


def sanitize_runtime_token(value: str) -> str:
    sanitized = SAFE_PATH_TOKEN_RE.sub("_", value).strip("._")
    return sanitized or "model"


def create_runtime_workspace(onnx_path: Path, dataset_count: int) -> Path:
    prefix = f"rknn_convert_{sanitize_runtime_token(onnx_path.stem)}_{dataset_count}_"
    return Path(tempfile.mkdtemp(prefix=prefix))


def collect_images(dataset_root: Path) -> list[Path]:
    images = [p for p in dataset_root.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_SUFFIXES]
    images.sort()
    return images


def collect_onnx_models(onnx_root: Path, recursive: bool) -> list[Path]:
    iterator = onnx_root.rglob("*") if recursive else onnx_root.glob("*")
    models = [path.resolve() for path in iterator if path.is_file() and path.suffix.lower() == ".onnx"]
    models.sort()
    return models


def evenly_sample(paths: list[Path], limit: int) -> list[Path]:
    if limit <= 0 or len(paths) <= limit:
        return paths
    step = len(paths) / limit
    indices = [min(math.floor(i * step), len(paths) - 1) for i in range(limit)]
    return [paths[i] for i in indices]


def write_dataset_file(images: list[Path], dataset_file: Path) -> None:
    dataset_file.parent.mkdir(parents=True, exist_ok=True)
    lines = [to_linux_path(str(path)) for path in images]
    dataset_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def image_signature(images: list[Path]) -> str:
    """确定性记录校准集内容，避免换图后误以为仍是同一量化模型。"""
    digest = hashlib.sha256()
    for path in images:
        digest.update(str(path.name).encode("utf-8"))
        digest.update(str(path.stat().st_size).encode("ascii"))
        digest.update(str(path.stat().st_mtime_ns).encode("ascii"))
    return digest.hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_calibration_images(images: list[Path], minimum: int) -> None:
    if len(images) < minimum:
        raise ValueError(
            f"Calibration dataset has only {len(images)} images; at least {minimum} are required "
            "for a stable INT8 export."
        )
    bad = [str(path) for path in images if path.stat().st_size < 1024]
    if bad:
        raise ValueError("Calibration images are suspiciously small: " + ", ".join(bad[:5]))


def default_stage_dir(onnx_path: Path, dataset_count: int) -> Path:
    return onnx_path.with_name(f"{onnx_path.stem}_calibration_{dataset_count}_images")


def stage_calibration_images(images: list[Path], stage_dir: Path) -> list[Path]:
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    staged_paths: list[Path] = []
    for index, image_path in enumerate(images):
        suffix = image_path.suffix.lower() or ".jpg"
        staged_path = stage_dir / f"{index:04d}{suffix}"
        shutil.copy2(image_path, staged_path)
        staged_paths.append(staged_path)
    return staged_paths


def cleanup_temporary_paths(paths: list[Path]) -> None:
    for path in paths:
        try:
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
        except FileNotFoundError:
            continue
        except OSError as exc:
            print(f"Warning: failed to remove temporary path {path}: {exc}", file=sys.stderr)


def tensor_dims(value_info: onnx.ValueInfoProto) -> list[int | str]:
    return [dim.dim_value or dim.dim_param or "?" for dim in value_info.type.tensor_type.shape.dim]


def producer_map(graph: onnx.GraphProto) -> dict[str, onnx.NodeProto]:
    mapping: dict[str, onnx.NodeProto] = {}
    for node in graph.node:
        for output_name in node.output:
            mapping[output_name] = node
    return mapping


def infer_shape_map(model: onnx.ModelProto) -> dict[str, list[int | str]]:
    shape_map: dict[str, list[int | str]] = {}
    try:
        inferred = shape_inference.infer_shapes(model)
        value_infos = (
            list(inferred.graph.value_info)
            + list(inferred.graph.input)
            + list(inferred.graph.output)
        )
        for value_info in value_infos:
            shape_map[value_info.name] = tensor_dims(value_info)
    except Exception:
        # Fallback 只使用 ONNX 文件里已经带出的形状，不再触发 shape inference。
        # --skip-shape-inference 失败重试路径和畸形图都能继续走 graph 导出。
        for value_info in (
            list(model.graph.value_info)
            + list(model.graph.input)
            + list(model.graph.output)
        ):
            shape_map[value_info.name] = tensor_dims(value_info)
        for initializer in model.graph.initializer:
            shape_map[initializer.name] = list(initializer.dims)
    return shape_map


def graph_value_info_names(model: onnx.ModelProto) -> set[str]:
    return {value_info.name for value_info in model.graph.value_info}


def graph_available_inputs(model: onnx.ModelProto) -> set[str]:
    names = {value_info.name for value_info in model.graph.input}
    names.update(initializer.name for initializer in model.graph.initializer)
    names.update(initializer.values.name for initializer in model.graph.sparse_initializer)
    return names


def value_info_map(model: onnx.ModelProto) -> dict[str, onnx.ValueInfoProto]:
    infos = list(model.graph.value_info) + list(model.graph.input) + list(model.graph.output)
    return {value_info.name: value_info for value_info in infos}


def tensor_elem_type(model: onnx.ModelProto, tensor_name: str) -> int:
    info = value_info_map(model).get(tensor_name)
    if info is None:
        return TensorProto.FLOAT
    elem_type = info.type.tensor_type.elem_type
    return elem_type or TensorProto.FLOAT


def is_topologically_sorted(model: onnx.ModelProto) -> bool:
    available = graph_available_inputs(model)
    for node in model.graph.node:
        if any(input_name and input_name not in available for input_name in node.input):
            return False
        available.update(output_name for output_name in node.output if output_name)
    return True


def topologically_sorted_model(model: onnx.ModelProto) -> onnx.ModelProto:
    sorted_model = onnx.ModelProto()
    sorted_model.CopyFrom(model)
    graph = sorted_model.graph
    available = graph_available_inputs(sorted_model)
    remaining = list(graph.node)
    sorted_nodes: list[onnx.NodeProto] = []

    while remaining:
        progressed = False
        next_remaining: list[onnx.NodeProto] = []
        for node in remaining:
            if all((not input_name) or input_name in available for input_name in node.input):
                sorted_nodes.append(node)
                available.update(output_name for output_name in node.output if output_name)
                progressed = True
            else:
                next_remaining.append(node)

        if not progressed:
            unresolved = []
            for node in next_remaining[:5]:
                missing = [input_name for input_name in node.input if input_name and input_name not in available]
                unresolved.append(f"{node.name or node.op_type}: {missing}")
            raise ValueError(
                "Unable to topologically sort ONNX graph; unresolved inputs: " + "; ".join(unresolved)
            )

        remaining = next_remaining

    del graph.node[:]
    graph.node.extend(sorted_nodes)
    return sorted_model


def constant_tensor_is_empty(node: onnx.NodeProto) -> bool:
    if node.op_type != "Constant":
        return False
    for attr in node.attribute:
        if attr.name == "value" and attr.HasField("t"):
            return any(dim == 0 for dim in attr.t.dims)
    return False


def resize_empty_roi_input(model: onnx.ModelProto, tensor_name: str, producers: dict[str, onnx.NodeProto]) -> bool:
    producer = producers.get(tensor_name)
    if producer is not None and producer.op_type == "Cast" and producer.input:
        producer = producers.get(producer.input[0])
    return producer is not None and constant_tensor_is_empty(producer)


def normalize_resize_empty_roi_inputs(model: onnx.ModelProto) -> list[str]:
    producers = producer_map(model.graph)
    rewritten = 0
    for node in model.graph.node:
        if node.op_type != "Resize" or len(node.input) < 3:
            continue
        roi_input = node.input[1]
        if not roi_input or not resize_empty_roi_input(model, roi_input, producers):
            continue
        node.input[1] = ""
        rewritten += 1
    if rewritten == 0:
        return []
    return [
        f"Normalized {rewritten} Resize empty roi input(s) to optional ONNX inputs for RKNN Toolkit compatibility."
    ]


def prepare_model_for_rknn(
    model: onnx.ModelProto,
    onnx_path: Path,
    workspace: Path,
    skip_shape_inference: bool = False,
) -> tuple[onnx.ModelProto, Path, list[str]]:
    prepared = model
    rewrite_required = False
    notes: list[str] = []

    resize_notes = normalize_resize_empty_roi_inputs(prepared)
    if resize_notes:
        rewrite_required = True
        notes.extend(resize_notes)

    if not is_topologically_sorted(prepared):
        prepared = topologically_sorted_model(prepared)
        rewrite_required = True
        notes.append("ONNX graph nodes were topologically reordered for RKNN Toolkit compatibility.")

    if not skip_shape_inference:
        before_value_infos = graph_value_info_names(prepared)
        try:
            inferred = shape_inference.infer_shapes(prepared)
            after_value_infos = graph_value_info_names(inferred)
            if after_value_infos != before_value_infos:
                rewrite_required = True
                notes.append("ONNX value_info entries were refreshed with shape inference.")
            prepared = inferred
        except Exception as exc:
            notes.append(f"Warning: ONNX shape inference failed while preparing RKNN input: {exc}")

    if not rewrite_required:
        return prepared, onnx_path, notes

    prepared_path = workspace / f"{sanitize_runtime_token(onnx_path.stem)}_rknn_prepared.onnx"
    onnx.save(prepared, str(prepared_path))
    return prepared, prepared_path, notes


def keepalive_name(base: str, suffix: str) -> str:
    return f"{base}/rknn_keepalive_{suffix}"


def output_appears_input_invariant(
    model: onnx.ModelProto,
    input_name: str,
    input_height: int,
    input_width: int,
    tensor_name: str,
    tensor_shape: list[int | str],
) -> bool | None:
    if not all(isinstance(dim, int) for dim in tensor_shape):
        return None

    try:
        import numpy as np
        import onnxruntime as ort
    except Exception:
        return None

    probe = onnx.ModelProto()
    probe.CopyFrom(model)
    del probe.graph.output[:]
    probe.graph.output.append(
        helper.make_tensor_value_info(tensor_name, tensor_elem_type(probe, tensor_name), tensor_shape)
    )

    try:
        session_options = ort.SessionOptions()
        session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        session = ort.InferenceSession(
            probe.SerializeToString(),
            sess_options=session_options,
            providers=["CPUExecutionProvider"],
        )
        rng = np.random.default_rng(20260516)
        inputs = [
            np.zeros((1, 3, input_height, input_width), dtype=np.float32),
            np.ones((1, 3, input_height, input_width), dtype=np.float32),
            rng.random((1, 3, input_height, input_width), dtype=np.float32),
        ]
        outputs = [session.run([tensor_name], {input_name: input_tensor})[0] for input_tensor in inputs]
    except Exception:
        return None

    base = outputs[0]
    max_delta = max(float(np.max(np.abs(output - base))) for output in outputs[1:])
    return max_delta <= 1.0e-6


def protect_constant_output_specs(
    model: onnx.ModelProto,
    output_specs: list[dict[str, object]],
    input_name: str,
    input_height: int,
    input_width: int,
    layout: str,
) -> tuple[onnx.ModelProto, list[dict[str, object]], list[str]]:
    if layout != "raw6":
        return model, output_specs, []
    if len(output_specs) != 6:
        return model, output_specs, []

    protected = onnx.ModelProto()
    protected.CopyFrom(model)
    graph = protected.graph
    info_by_name = value_info_map(protected)
    protected_specs = [dict(spec) for spec in output_specs]

    source_spec = output_specs[3]
    target_spec = output_specs[5]
    source_shape = source_spec.get("shape")
    target_shape = target_spec.get("shape")
    if (
        not isinstance(source_shape, list)
        or not isinstance(target_shape, list)
        or len(source_shape) != 4
        or len(target_shape) != 4
        or source_shape[0] != target_shape[0]
        or source_shape[1] != target_shape[1]
        or not isinstance(source_shape[2], int)
        or not isinstance(source_shape[3], int)
        or not isinstance(target_shape[2], int)
        or not isinstance(target_shape[3], int)
        or source_shape[2] % target_shape[2] != 0
        or source_shape[3] % target_shape[3] != 0
    ):
        return model, output_specs, []

    ratio_h = source_shape[2] // target_shape[2]
    ratio_w = source_shape[3] // target_shape[3]
    if ratio_h < 1 or ratio_w < 1:
        return model, output_specs, []

    source_name = str(source_spec["tensor_name"])
    target_name = str(target_spec["tensor_name"])
    pool_name = keepalive_name(target_name, "pool_from_prev_cls")
    scale_const_name = keepalive_name(target_name, "scale_const")
    perturb_name = keepalive_name(target_name, "perturb")
    alias_name = keepalive_name(target_name, "dynamic_output")

    if ratio_h == 1 and ratio_w == 1:
        pooled_name = source_name
    else:
        graph.node.append(
            helper.make_node(
                "AveragePool",
                inputs=[source_name],
                outputs=[pool_name],
                name=keepalive_name(target_name, "avgpool_from_prev_cls"),
                kernel_shape=[ratio_h, ratio_w],
                strides=[ratio_h, ratio_w],
            )
        )
        pooled_name = pool_name

    graph.node.extend(
        [
            helper.make_node(
                "Constant",
                inputs=[],
                outputs=[scale_const_name],
                name=keepalive_name(target_name, "scale_const_node"),
                value=helper.make_tensor(
                    scale_const_name + "_value",
                    TensorProto.FLOAT,
                    [1],
                    [RAW_OUTPUT_KEEPALIVE_SCALE],
                ),
            ),
            helper.make_node(
                "Mul",
                inputs=[pooled_name, scale_const_name],
                outputs=[perturb_name],
                name=keepalive_name(target_name, "perturb_mul"),
            ),
            helper.make_node(
                "Add",
                inputs=[target_name, perturb_name],
                outputs=[alias_name],
                name=keepalive_name(target_name, "dynamic_add"),
            ),
        ]
    )

    elem_type = tensor_elem_type(protected, target_name)
    original_info = info_by_name.get(target_name)
    if original_info is not None:
        graph.value_info.append(helper.make_tensor_value_info(alias_name, elem_type, tensor_dims(original_info)))
    else:
        graph.value_info.append(helper.make_tensor_value_info(alias_name, elem_type, target_shape))

    protected_specs[5]["tensor_name"] = alias_name
    protected_specs[5]["node_name"] = alias_name
    notes = [
        "Added RKNN raw6 fold guard for the smallest class head using a tiny pooled dynamic alias."
    ]
    return protected, protected_specs, notes


def first_tensor_input(model: onnx.ModelProto) -> onnx.ValueInfoProto:
    for value_info in model.graph.input:
        dims = tensor_dims(value_info)
        if len(dims) == 4:
            return value_info
    raise ValueError("No 4D input tensor found in ONNX model.")


def parse_names(metadata: dict[str, str]) -> dict[int, str]:
    raw_names = metadata.get("names")
    if not raw_names:
        return {}
    try:
        parsed = ast.literal_eval(raw_names)
    except Exception:
        return {}
    if isinstance(parsed, dict):
        normalized: dict[int, str] = {}
        for key, value in parsed.items():
            try:
                normalized[int(key)] = str(value)
            except Exception:
                continue
        return normalized
    return {}


def detect_head_prefix(model: onnx.ModelProto) -> str:
    producers = producer_map(model.graph)
    for output in model.graph.output:
        node = producers.get(output.name)
        if not node:
            continue
        match = MODEL_PREFIX_RE.match(node.name)
        if match:
            return match.group("prefix")

    highest = -1
    best_prefix = ""
    for node in model.graph.node:
        match = MODEL_PREFIX_RE.match(node.name)
        if not match:
            continue
        prefix = match.group("prefix")
        index = int(prefix.split(".")[1])
        if index > highest:
            highest = index
            best_prefix = prefix
    if not best_prefix:
        raise ValueError("Unable to locate detection head prefix from ONNX graph.")
    return best_prefix


def infer_raw_head_outputs(
    model: onnx.ModelProto, shape_map: dict[str, list[int | str]]
) -> tuple[str, list[dict[str, object]]]:
    head_prefix = detect_head_prefix(model)
    grouped: dict[int, dict[str, dict[str, object]]] = {}

    for node in model.graph.node:
        if node.op_type != "Conv":
            continue
        match = HEAD_CONV_RE.match(node.name)
        if not match or match.group("head_prefix") != head_prefix:
            continue
        output_name = node.output[0]
        shape = shape_map.get(output_name)
        if not shape or len(shape) != 4:
            continue
        scale = int(match.group("scale"))
        branch = "box" if match.group("branch") == "2" else "cls"
        grouped.setdefault(scale, {})[branch] = {
            "node_name": node.name,
            "tensor_name": output_name,
            "shape": shape,
        }

    if not grouped:
        raise ValueError(
            "No raw detect heads were found. This script currently supports Ultralytics detect ONNX graphs "
            "that expose final head convolutions as cv2.<scale>.2 and cv3.<scale>.2."
        )

    output_specs: list[dict[str, object]] = []
    missing: list[int] = []
    for scale in sorted(grouped):
        entry = grouped[scale]
        if "box" not in entry or "cls" not in entry:
            missing.append(scale)
            continue
        output_specs.append(entry["box"])
        output_specs.append(entry["cls"])

    if missing:
        raise ValueError(f"Incomplete raw head pairs were found for scales: {missing}")

    return head_prefix, output_specs


def infer_yolov5_raw_head_outputs(
    model: onnx.ModelProto, shape_map: dict[str, list[int | str]]
) -> tuple[str, list[dict[str, object]]]:
    head_prefix = detect_head_prefix(model)
    output_specs: dict[int, dict[str, object]] = {}

    for node in model.graph.node:
        if node.op_type != "Conv":
            continue
        match = YOLOV5_DETECT_CONV_RE.match(node.name)
        if not match or match.group("head_prefix") != head_prefix:
            continue
        output_name = node.output[0]
        shape = shape_map.get(output_name)
        if not shape or len(shape) != 4:
            continue
        scale = int(match.group("scale"))
        output_specs[scale] = {
            "node_name": node.name,
            "tensor_name": output_name,
            "shape": shape,
        }

    if not output_specs:
        raise ValueError(
            "No classic YOLOv5 raw detect heads were found. This script expects final detect convs like "
            "m.0/Conv, m.1/Conv, m.2/Conv."
        )

    return head_prefix, [output_specs[index] for index in sorted(output_specs)]


def infer_raw9_head_outputs(
    model: onnx.ModelProto, shape_map: dict[str, list[int | str]]
) -> tuple[str, list[dict[str, object]]]:
    head_prefix, raw6_outputs = infer_raw_head_outputs(model, shape_map)
    aux_specs: dict[int, dict[str, object]] = {}

    for node in model.graph.node:
        if node.op_type != "ReduceSum":
            continue
        match = RAW9_AUX_RE.match(node.name)
        if not match or match.group("head_prefix") != head_prefix:
            continue
        output_name = node.output[0]
        shape = shape_map.get(output_name)
        if not shape or len(shape) != 4:
            continue
        scale = int(match.group("scale"))
        aux_specs[scale] = {
            "node_name": node.name,
            "tensor_name": output_name,
            "shape": shape,
        }

    if not aux_specs:
        raise ValueError(
            "No raw9 auxiliary heads were found. This script expects ReduceSum nodes named like "
            "raw9_aux.<scale>/ReduceSum to exist alongside standard Ultralytics raw6 detect heads."
        )

    output_specs: list[dict[str, object]] = []
    missing: list[int] = []
    for scale_index, (box_spec, cls_spec) in enumerate(zip(raw6_outputs[0::2], raw6_outputs[1::2])):
        aux_spec = aux_specs.get(scale_index)
        if aux_spec is None:
            missing.append(scale_index)
            continue
        output_specs.append(box_spec)
        output_specs.append(cls_spec)
        output_specs.append(aux_spec)

    if missing:
        raise ValueError(f"Incomplete raw9 auxiliary outputs were found for scales: {missing}")

    return head_prefix, output_specs


def infer_graph_output_specs(
    model: onnx.ModelProto, shape_map: dict[str, list[int | str]]
) -> list[dict[str, object]]:
    producers = producer_map(model.graph)
    output_specs: list[dict[str, object]] = []

    for output in model.graph.output:
        shape = shape_map.get(output.name) or tensor_dims(output)
        producer = producers.get(output.name)
        output_specs.append(
            {
                "node_name": producer.name if producer else output.name,
                "tensor_name": output.name,
                "shape": shape,
            }
        )

    if not output_specs:
        raise ValueError("The ONNX graph does not expose any outputs.")

    return output_specs


def is_yolo26_e2e_output_shape(shape: object) -> bool:
    if not isinstance(shape, list) or len(shape) != 3:
        return False
    batch, first, second = shape
    if isinstance(batch, int) and batch != 1:
        return False
    if second == 6 and isinstance(first, int) and 1 <= first <= 1000:
        return True
    if first == 6 and isinstance(second, int) and 1 <= second <= 1000:
        return True
    return False


def transpose_perm(node: onnx.NodeProto) -> list[int] | None:
    for attr in node.attribute:
        if attr.name == "perm":
            return list(attr.ints)
    return None


def graph_output_dependency_values(model: onnx.ModelProto) -> set[str]:
    producers = producer_map(model.graph)
    seen_nodes: set[int] = set()
    seen_values = {output.name for output in model.graph.output}
    pending_values = list(seen_values)

    while pending_values:
        value_name = pending_values.pop()
        node = producers.get(value_name)
        if node is None:
            continue
        node_id = id(node)
        if node_id in seen_nodes:
            continue
        seen_nodes.add(node_id)
        for output_name in node.output:
            if output_name:
                seen_values.add(output_name)
        for input_name in node.input:
            if input_name and input_name not in seen_values:
                seen_values.add(input_name)
                pending_values.append(input_name)

    return seen_values


def infer_yolo26_e2e_raw_output(
    model: onnx.ModelProto,
    shape_map: dict[str, list[int | str]],
    graph_outputs: list[dict[str, object]],
) -> tuple[str | None, list[dict[str, object]]]:
    if not any(is_yolo26_e2e_output_shape(spec.get("shape")) for spec in graph_outputs):
        raise ValueError("The graph outputs do not look like YOLO26 end-to-end [1, max_det, 6] outputs.")

    producers = producer_map(model.graph)
    dependency_values = graph_output_dependency_values(model)
    consumers: dict[str, list[onnx.NodeProto]] = {}
    for node in model.graph.node:
        for input_name in node.input:
            if input_name:
                consumers.setdefault(input_name, []).append(node)
    candidates: list[tuple[int, str | None, list[dict[str, object]]]] = []

    for node in model.graph.node:
        if node.op_type != "Transpose" or transpose_perm(node) != [0, 2, 1]:
            continue
        if not node.input or not node.output or node.output[0] not in dependency_values:
            continue

        tensor_name = node.input[0]
        shape = shape_map.get(tensor_name)
        if (
            not isinstance(shape, list)
            or len(shape) != 3
            or not isinstance(shape[1], int)
            or not isinstance(shape[2], int)
            or shape[1] < 5
            or shape[1] > 256
            or shape[2] <= 0
        ):
            continue

        producer = producers.get(tensor_name)
        prefix_match = MODEL_PREFIX_RE.match(node.name)
        if producer is not None and producer.op_type == "Concat" and len(producer.input) >= 2:
            split_specs: list[dict[str, object]] = []
            for input_name in producer.input[:2]:
                input_shape = shape_map.get(input_name)
                input_producer = producers.get(input_name)
                if (
                    not isinstance(input_shape, list)
                    or len(input_shape) != 3
                    or input_shape[0] != shape[0]
                    or not isinstance(input_shape[1], int)
                    or not isinstance(input_shape[2], int)
                    or input_shape[2] != shape[2]
                ):
                    split_specs = []
                    break
                split_specs.append(
                    {
                        "node_name": input_producer.name if input_producer else input_name,
                        "tensor_name": input_name,
                        "shape": input_shape,
                        "box_format": "xyxy",
                    }
                )
            if (
                len(split_specs) == 2
                and split_specs[0]["shape"][1] == 4
                and isinstance(split_specs[1]["shape"][1], int)
                and split_specs[1]["shape"][1] > 0
            ):
                candidates.append((shape[2], prefix_match.group("prefix") if prefix_match else None, split_specs))
                continue

        transpose_consumers = consumers.get(node.output[0], [])
        split_node = next(
            (
                consumer
                for consumer in transpose_consumers
                if consumer.op_type == "Split" and len(consumer.output) >= 2
            ),
            None,
        )
        if split_node is not None:
            split_specs = []
            for output_name in split_node.output[:2]:
                output_shape = shape_map.get(output_name)
                if (
                    not isinstance(output_shape, list)
                    or len(output_shape) != 3
                    or output_shape[0] != shape[0]
                    or not isinstance(output_shape[1], int)
                    or output_shape[1] != shape[2]
                    or not isinstance(output_shape[2], int)
                ):
                    split_specs = []
                    break
                split_specs.append(
                    {
                        "node_name": split_node.name,
                        "tensor_name": output_name,
                        "shape": output_shape,
                        "box_format": "xyxy",
                    }
                )
            if (
                len(split_specs) == 2
                and split_specs[0]["shape"][2] == 4
                and isinstance(split_specs[1]["shape"][2], int)
                and split_specs[1]["shape"][2] > 0
            ):
                candidates.append((shape[2], prefix_match.group("prefix") if prefix_match else None, split_specs))
                continue

        candidates.append(
            (
                shape[2],
                prefix_match.group("prefix") if prefix_match else None,
                [
                    {
                        "node_name": producer.name if producer else tensor_name,
                        "tensor_name": tensor_name,
                        "shape": shape,
                        "box_format": "xyxy",
                    }
                ],
            )
        )

    if not candidates:
        raise ValueError(
            "No YOLO26 raw tensor was found before the end-to-end Transpose([0, 2, 1]) postprocess."
        )

    candidates.sort(key=lambda item: item[0], reverse=True)
    _, head_prefix, output_specs = candidates[0]
    return head_prefix, output_specs


def is_direct_regression_raw6(output_specs: list[dict[str, object]]) -> bool:
    if len(output_specs) != 6:
        return False
    for box_spec in output_specs[0::2]:
        shape = box_spec.get("shape")
        if not isinstance(shape, list) or len(shape) < 2 or shape[1] != 4:
            return False
    return True


def select_output_specs(
    model: onnx.ModelProto, shape_map: dict[str, list[int | str]], output_layout: str
) -> tuple[str, str | None, list[dict[str, object]]]:
    graph_outputs = infer_graph_output_specs(model, shape_map)
    raw9_head_prefix: str | None = None
    raw9_outputs: list[dict[str, object]] = []
    raw9_error: ValueError | None = None
    raw6_head_prefix: str | None = None
    raw6_outputs: list[dict[str, object]] = []
    raw6_error: ValueError | None = None
    raw3_head_prefix: str | None = None
    raw3_outputs: list[dict[str, object]] = []
    raw3_error: ValueError | None = None
    yolo26_head_prefix: str | None = None
    yolo26_outputs: list[dict[str, object]] = []
    yolo26_error: ValueError | None = None

    try:
        yolo26_head_prefix, yolo26_outputs = infer_yolo26_e2e_raw_output(model, shape_map, graph_outputs)
    except ValueError as exc:
        yolo26_error = exc

    try:
        raw9_head_prefix, raw9_outputs = infer_raw9_head_outputs(model, shape_map)
    except ValueError as exc:
        raw9_error = exc

    try:
        raw6_head_prefix, raw6_outputs = infer_raw_head_outputs(model, shape_map)
    except ValueError as exc:
        raw6_error = exc

    try:
        raw3_head_prefix, raw3_outputs = infer_yolov5_raw_head_outputs(model, shape_map)
    except ValueError as exc:
        raw3_error = exc

    if output_layout == "raw9":
        if not raw9_outputs:
            raise raw9_error or ValueError("Raw9 auxiliary detect heads were not found in the ONNX graph.")
        return "raw9", raw9_head_prefix, raw9_outputs

    if output_layout == "raw6":
        if not raw6_outputs:
            raise raw6_error or ValueError("Raw split detect heads were not found in the ONNX graph.")
        return "raw6", raw6_head_prefix, raw6_outputs

    if output_layout == "raw3":
        if not raw3_outputs:
            raise raw3_error or ValueError("Classic YOLOv5 raw detect heads were not found in the ONNX graph.")
        return "raw3", raw3_head_prefix, raw3_outputs

    if output_layout in {"yolo26", "v26"}:
        if not yolo26_outputs:
            raise yolo26_error or ValueError("YOLO26 end-to-end raw output was not found in the ONNX graph.")
        return "yolo26", yolo26_head_prefix, yolo26_outputs

    if output_layout == "graph":
        return "graph", None, graph_outputs

    if raw6_outputs:
        return "raw6", raw6_head_prefix, raw6_outputs

    if raw9_outputs:
        return "raw9", raw9_head_prefix, raw9_outputs

    if yolo26_outputs:
        return "yolo26", yolo26_head_prefix, yolo26_outputs

    if raw3_outputs:
        return "raw3", raw3_head_prefix, raw3_outputs

    return "graph", None, graph_outputs


def detect_input_spec(
    model: onnx.ModelProto, input_height: int | None, input_width: int | None
) -> tuple[str, int, int]:
    input_value = first_tensor_input(model)
    dims = tensor_dims(input_value)
    input_name = input_value.name

    detected_height = dims[2] if len(dims) > 2 else None
    detected_width = dims[3] if len(dims) > 3 else None

    if input_height is None:
        if not isinstance(detected_height, int):
            raise ValueError("Model input height is dynamic. Please provide --input-height explicitly.")
        input_height = detected_height
    if input_width is None:
        if not isinstance(detected_width, int):
            raise ValueError("Model input width is dynamic. Please provide --input-width explicitly.")
        input_width = detected_width

    return input_name, input_height, input_width


def default_output_path(onnx_path: Path, target_platform: str) -> Path:
    return onnx_path.with_name(f"{onnx_path.stem}_raw_int8_{target_platform}.rknn")


def default_dataset_file(onnx_path: Path, dataset_count: int) -> Path:
    return onnx_path.with_name(f"{onnx_path.stem}_calibration_{dataset_count}.txt")


def default_batch_output_path(onnx_path: Path, source_root: Path, output_root: Path, target_platform: str) -> Path:
    relative_dir = onnx_path.relative_to(source_root).parent
    return (output_root / relative_dir / default_output_path(onnx_path, target_platform).name).resolve()


def default_batch_dataset_file(onnx_path: Path, source_root: Path, dataset_root: Path, dataset_count: int) -> Path:
    relative_dir = onnx_path.relative_to(source_root).parent
    return (dataset_root / relative_dir / default_dataset_file(onnx_path, dataset_count).name).resolve()


def resolve_batch_root(path_arg: str | None) -> Path | None:
    if path_arg is None:
        return None
    root = runtime_path(path_arg).resolve()
    if root.exists() and root.is_file():
        raise ValueError(f"Batch mode expects a directory path, got file: {root}")
    return root


def parse_triplet(value: str) -> list[list[float]]:
    parts = [float(item.strip()) for item in value.split(",") if item.strip()]
    if len(parts) != 3:
        raise ValueError(f"Expected exactly 3 comma-separated values, got: {value}")
    return [parts]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert one detect ONNX model, or every ONNX model in a directory, to INT8 RKNN."
    )
    parser.add_argument(
        "--onnx",
        default="models/onnx/valorant_256_v26n.onnx",
        help="Path to the source ONNX model, or a directory containing ONNX models.",
    )
    parser.add_argument(
        "--dataset-root",
        default="test_images",
        help="Calibration image root. Windows paths are accepted and converted for WSL use.",
    )
    parser.add_argument(
        "--dataset-file",
        default=None,
        help="Calibration manifest path for single-model mode, or manifest directory root for batch mode.",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="Output RKNN file path for single-model mode, or output directory root for batch mode.",
    )
    parser.add_argument(
        "--target-platform",
        default="rk3588",
        help="RKNN target platform, for example rk3588 or rk3568.",
    )
    parser.add_argument(
        "--dataset-count",
        type=int,
        default=100,
        help="How many calibration images to sample evenly from the dataset root (INT8 default: 100).",
    )
    parser.add_argument(
        "--minimum-calibration-images",
        type=int,
        default=32,
        help="Minimum usable images required before an INT8 build is allowed.",
    )
    parser.add_argument(
        "--quantized-dtype",
        choices=["w8a8", "w8a16", "w16a16i", "w16a16i_dfp", "w4a16", "asymmetric_quantized-8", "asymmetric_quantized-16"],
        default="w8a8",
        help="RKNN quantization scheme passed to RKNN.config() (rknn_toolkit2 2.x names; legacy asymmetric names are accepted and auto-mapped).",
    )
    parser.add_argument(
        "--no-quantization",
        action="store_true",
        help="Export a floating-point RKNN model for A/B comparison; skips calibration requirements.",
    )
    parser.add_argument("--input-width", type=int, default=None, help="Optional input width override.")
    parser.add_argument("--input-height", type=int, default=None, help="Optional input height override.")
    parser.add_argument(
        "--mean-values",
        default="0,0,0",
        help="Comma-separated mean values applied by RKNN config.",
    )
    parser.add_argument(
        "--std-values",
        default="255,255,255",
        help="Comma-separated std values applied by RKNN config.",
    )
    parser.add_argument(
        "--output-layout",
        choices=["auto", "yolo26", "v26", "raw9", "raw6", "raw3", "graph"],
        default="auto",
        help=(
            "Which ONNX outputs to export. "
            "'auto' prefers Ultralytics raw split heads when a 6-output conversion is possible, "
            "then raw9 auxiliary heads, then YOLO26 end-to-end internal raw output, then classic YOLOv5 raw detect convs, "
            "otherwise keeps the graph outputs. "
            "'yolo26' forces the internal [1,4+classes,N] raw tensor before the end-to-end postprocess; "
            "'raw9' forces per-scale box/cls/aux triples; "
            "'raw6' forces per-scale box/cls pairs; 'raw3' forces classic YOLOv5 per-scale detect convs; "
            "'graph' preserves the ONNX graph outputs as-is."
        ),
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="When --onnx points to a directory, scan subdirectories recursively for ONNX files.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only parse the ONNX graph and print the selected outputs without building RKNN.",
    )
    parser.add_argument(
        "--preserve-io",
        action="store_true",
        help=(
            "Backward-compatible option for older tools. When set, keep the original ONNX graph outputs "
            "and force graph-mode export."
        ),
    )
    parser.add_argument(
        "--skip-shape-inference",
        action="store_true",
        help="Skip ONNX shape inference during preparation (fallback path used by the web retry).",
    )
    return parser.parse_args()


def convert_one_model(
    args: argparse.Namespace,
    onnx_path: Path,
    dataset_root: Path,
    output_path: Path | None = None,
    dataset_file: Path | None = None,
) -> int:
    if not onnx_path.exists():
        raise FileNotFoundError(f"ONNX model not found: {onnx_path}")

    model = onnx.load(str(onnx_path))
    runtime_workspace = create_runtime_workspace(onnx_path, args.dataset_count)
    stage_dir = (runtime_workspace / "calibration_images").resolve()
    runtime_dataset_file = (runtime_workspace / "calibration.txt").resolve()
    temporary_paths = [runtime_workspace]
    model, rknn_onnx_path, prepare_notes = prepare_model_for_rknn(
        model,
        onnx_path,
        runtime_workspace,
        args.skip_shape_inference,
    )
    metadata = {item.key: item.value for item in model.metadata_props}

    # rknn_toolkit2 2.x dropped the legacy asymmetric/dynamic names that the
    # old toolkit accepted. Map them to the equivalent modern scheme before
    # passing to RKNN.config() so older command lines keep working.
    legacy_mapping = {
        "asymmetric_quantized-8": "w8a8",
        "asymmetric_quantized-16": "w16a16i",
        "dynamic_fixed_point-8": "w8a8",
    }
    quant_scheme = args.quantized_dtype
    if quant_scheme in legacy_mapping:
        print(
            f"Note: quantized dtype {quant_scheme!r} is a legacy rknn_toolkit1 name, "
            f"mapping to {legacy_mapping[quant_scheme]!r} for rknn_toolkit2.",
            file=sys.stderr,
        )
        quant_scheme = legacy_mapping[quant_scheme]
    args.quantized_dtype = quant_scheme

    # Platform capability guard (verified on rknn_toolkit2 2.3.2 / RK3588):
    #   - w8a16 / w4a16  -> rejected by config() on rk3588 (ValueError)
    #   - w16a16i        -> converts OK but current runtime fails with
    #                       "failed to submit!" (op id 1 Conv), so it is unusable
    #                       on this target for now.
    if args.target_platform.startswith(("rk3588", "rk356")):
        if args.quantized_dtype in ("w8a16", "w4a16"):
            raise ValueError(
                f"quantized_dtype={args.quantized_dtype} is not supported on "
                f"{args.target_platform} by rknn_toolkit2. Use w8a8 instead."
            )
        if args.quantized_dtype in ("w16a16i", "w16a16i_dfp"):
            print(
                "Warning: w16a16i converts on rk3588 but the bundled runtime "
                "reports 'failed to submit' at inference; only w8a8 is known to run.",
                file=sys.stderr,
            )
    class_names = parse_names(metadata)
    input_name, input_height, input_width = detect_input_spec(model, args.input_height, args.input_width)
    shape_map = infer_shape_map(model)
    output_layout, head_prefix, output_specs = select_output_specs(model, shape_map, args.output_layout)
    if args.preserve_io:
        output_layout = "graph"
        head_prefix = None
        output_specs = infer_graph_output_specs(model, shape_map)
    else:
        model, output_specs, keepalive_notes = protect_constant_output_specs(
            model,
            output_specs,
            input_name,
            input_height,
            input_width,
            output_layout,
        )
        if keepalive_notes:
            prepare_notes.extend(keepalive_notes)
            rknn_onnx_path = runtime_workspace / f"{sanitize_runtime_token(onnx_path.stem)}_rknn_prepared.onnx"
            model = shape_inference.infer_shapes(model)
            onnx.save(model, str(rknn_onnx_path))

    output_path = output_path or (
        runtime_path(args.output).resolve() if args.output else default_output_path(onnx_path, args.target_platform).resolve()
    )
    dataset_file = dataset_file or (runtime_path(args.dataset_file).resolve() if args.dataset_file else None)

    print(f"ONNX: {onnx_path}")
    for note in prepare_notes:
        print(note)
    if rknn_onnx_path != onnx_path:
        print(f"Prepared ONNX for RKNN: {rknn_onnx_path}")
    print(f"Selected output layout: {output_layout}")
    if head_prefix:
        print(f"Head prefix: {head_prefix}")
    print(f"Task: {metadata.get('task', 'unknown')}")
    if args.preserve_io:
        print(
            "Note: --preserve-io is enabled, so the original ONNX graph outputs are exported as-is.",
            file=sys.stderr,
        )
    if output_layout == "raw6" and is_direct_regression_raw6(output_specs):
        print(
            "Note: this model uses 4-channel direct-regression box heads. If an older raw6 RKNN shows oversized boxes, "
            "rebuild it with the current converter/runtime before switching to graph.",
            file=sys.stderr,
        )
    if class_names:
        print(f"Classes ({len(class_names)}): {class_names}")
    print(f"Input tensor: {input_name}")
    print(f"Expected RKNN input: int8[1,3,{input_height},{input_width}]")
    print("Expected RKNN outputs:")
    for spec in output_specs:
        print(f"  {spec['tensor_name']} -> {spec['shape']}")

    if args.dry_run:
        print("Dry run requested, skipping calibration set generation and RKNN build.")
        return 0

    if not dataset_root.exists():
        raise FileNotFoundError(f"Calibration dataset root not found: {dataset_root}")

    all_images = collect_images(dataset_root)
    if not all_images:
        raise FileNotFoundError(f"No calibration images found under: {dataset_root}")

    if not args.no_quantization:
        # 当用户明确要求少量校准图（--dataset-count 5）时，最低门槛随实际抽样数
        # 收敛，避免"用户给了 10 张却硬说至少 32 张"的隐性失败；大批量仍保留
        # 32 张的稳定性底线。
        requested_count = max(1, args.dataset_count)
        validate_calibration_images(
            all_images, min(args.minimum_calibration_images, requested_count))

    rknn = None
    try:
        sampled_images = evenly_sample(all_images, requested_count)
        staged_images = stage_calibration_images(sampled_images, stage_dir)
        write_dataset_file(staged_images, runtime_dataset_file)
        if dataset_file is not None:
            write_dataset_file(staged_images, dataset_file)

        mean_values = parse_triplet(args.mean_values)
        std_values = parse_triplet(args.std_values)

        print(f"Dataset root: {dataset_root}")
        print(f"Calibration images discovered: {len(all_images)}")
        print(f"Calibration images used: {len(sampled_images)}")
        print(f"Calibration signature: {image_signature(sampled_images)}")
        print(f"Calibration stage dir: {stage_dir}")
        print(f"Calibration manifest: {runtime_dataset_file}")
        if dataset_file is not None:
            print(f"Calibration manifest copy: {dataset_file}")
        print(f"Target platform: {args.target_platform}")

        from rknn.api import RKNN

        rknn = RKNN(verbose=False)
        ret = rknn.config(
            target_platform=args.target_platform,
            mean_values=mean_values,
            std_values=std_values,
            quantized_dtype=args.quantized_dtype,
        )
        if ret != 0:
            print(f"rknn.config failed: {ret}", file=sys.stderr)
            return ret

        ret = rknn.load_onnx(
            model=str(rknn_onnx_path).replace("\\", "/"),
            inputs=[input_name],
            input_size_list=[[1, 3, input_height, input_width]],
            outputs=[str(spec["tensor_name"]) for spec in output_specs],
        )
        if ret != 0:
            print(f"rknn.load_onnx failed: {ret}", file=sys.stderr)
            return ret

        build_kwargs = {"do_quantization": not args.no_quantization}
        if not args.no_quantization:
            build_kwargs["dataset"] = str(runtime_dataset_file).replace("\\", "/")
        ret = rknn.build(**build_kwargs)
        if ret != 0:
            print(f"rknn.build failed: {ret}", file=sys.stderr)
            return ret

        output_path.parent.mkdir(parents=True, exist_ok=True)
        ret = rknn.export_rknn(str(output_path).replace("\\", "/"))
        if ret != 0:
            print(f"rknn.export_rknn failed: {ret}", file=sys.stderr)
            return ret

        if not output_path.exists() or output_path.stat().st_size < 4096:
            raise RuntimeError(f"RKNN export produced an invalid artifact: {output_path}")
        artifact_sha256 = file_sha256(output_path)
        metadata_path = output_path.with_suffix(output_path.suffix + ".json")
        metadata_path.write_text(json.dumps({
            "source_onnx": str(onnx_path),
            "output": str(output_path),
            "output_size_bytes": output_path.stat().st_size,
            "output_sha256": artifact_sha256,
            "target_platform": args.target_platform,
            "quantized": not args.no_quantization,
            "quantized_dtype": None if args.no_quantization else args.quantized_dtype,
            "dataset_count": len(sampled_images),
            "calibration_signature": image_signature(sampled_images),
            "input_name": input_name,
            "input_shape": [1, 3, input_height, input_width],
            "output_layout": output_layout,
        }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"RKNN exported successfully: {output_path}")
        print(f"Conversion metadata: {metadata_path}")
        return 0
    finally:
        if rknn is not None:
            rknn.release()
        cleanup_temporary_paths(temporary_paths)


def main() -> int:
    args = parse_args()

    onnx_input = runtime_path(args.onnx).resolve()
    dataset_root = runtime_path(args.dataset_root).resolve()

    if onnx_input.is_file():
        return convert_one_model(args, onnx_input, dataset_root)

    if not onnx_input.is_dir():
        raise FileNotFoundError(f"ONNX model or directory not found: {onnx_input}")

    onnx_models = collect_onnx_models(onnx_input, args.recursive)
    if not onnx_models:
        raise FileNotFoundError(f"No ONNX models were found under: {onnx_input}")

    output_root = resolve_batch_root(args.output)
    dataset_file_root = resolve_batch_root(args.dataset_file)

    successes: list[Path] = []
    failures: list[tuple[Path, str]] = []

    print(f"Batch source: {onnx_input}")
    print(f"ONNX models found: {len(onnx_models)}")
    print(f"Recursive scan: {args.recursive}")

    for index, onnx_path in enumerate(onnx_models, start=1):
        print()
        print(f"[{index}/{len(onnx_models)}] Converting: {onnx_path}")

        batch_output_path = (
            default_batch_output_path(onnx_path, onnx_input, output_root, args.target_platform)
            if output_root is not None
            else default_output_path(onnx_path, args.target_platform).resolve()
        )
        batch_dataset_file = (
            default_batch_dataset_file(onnx_path, onnx_input, dataset_file_root, args.dataset_count)
            if dataset_file_root is not None
            else default_dataset_file(onnx_path, args.dataset_count).resolve()
        )

        try:
            ret = convert_one_model(
                args,
                onnx_path=onnx_path,
                dataset_root=dataset_root,
                output_path=batch_output_path,
                dataset_file=batch_dataset_file,
            )
            if ret == 0:
                successes.append(onnx_path)
            else:
                failures.append((onnx_path, f"Return code: {ret}"))
        except Exception as exc:
            failures.append((onnx_path, f"{type(exc).__name__}: {exc}"))
            print(f"Conversion failed: {exc}", file=sys.stderr)

    print()
    print("Batch summary:")
    print(f"  Success: {len(successes)}")
    print(f"  Failed: {len(failures)}")

    if failures:
        print("Failed models:")
        for onnx_path, reason in failures:
            print(f"  {onnx_path} -> {reason}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
