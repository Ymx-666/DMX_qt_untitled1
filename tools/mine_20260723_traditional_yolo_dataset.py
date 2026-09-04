#!/usr/bin/env python3
"""Mine the complete 20260723 sky dataset with traditional + YOLO confirm.

Input windows come from ``generate_20260723_sky_512_dataset.py``.  Traditional
point-target detection runs on every sky-intersecting 512x512 window.  Results
are mapped back to 4096x4096 frame coordinates and then to the 16-tile
panorama.  For each stream and revolution:

1. rank all traditional candidates;
2. apply circular panorama-coordinate NMS;
3. send at most eight candidates to YOLOv26s weights2;
4. save only candidates whose center-weighted drone score passes the configured
   threshold and dominates the bird score.

Saved 512x512 images are recropped from the oriented raw source frame so the
traditional candidate is centered and grid-window boundaries do not introduce
black padding.  Black is used only when the centered crop crosses a true raw
frame boundary.

The run is resumable.  ``group_summary.csv`` is the completion journal for each
stream/revolution group, while ``manifest.csv`` is appended before a group is
marked complete.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Mapping, Sequence, Set, Tuple

import cv2
import numpy as np


DEFAULT_DATASET_ROOT = Path("/mnt/dmx4t/DMX_yangben/20260723")
DEFAULT_OUT_DIR = DEFAULT_DATASET_ROOT / "traditional_yolo_dataset"
DEFAULT_REFERENCE = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg")
DEFAULT_MODEL = Path(
    "/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx"
)

MANIFEST_FIELDS = [
    "file",
    "dataset_crop",
    "raw_source",
    "stream",
    "time_group",
    "round",
    "source_index",
    "world_tile",
    "grid_x",
    "grid_y",
    "frame_x",
    "frame_y",
    "pano_x",
    "pano_y",
    "traditional_frame_x1",
    "traditional_frame_y1",
    "traditional_frame_x2",
    "traditional_frame_y2",
    "traditional_roi_x1",
    "traditional_roi_y1",
    "traditional_roi_x2",
    "traditional_roi_y2",
    "traditional_area",
    "traditional_score",
    "traditional_response",
    "traditional_contrast",
    "traditional_center_ring",
    "traditional_template_corr",
    "traditional_aspect",
    "traditional_compactness",
    "yolo_drone_raw",
    "yolo_drone_weighted",
    "yolo_bird_raw",
    "yolo_bird_weighted",
    "yolo_center_distance",
    "yolo_x1",
    "yolo_y1",
    "yolo_x2",
    "yolo_y2",
]

GROUP_FIELDS = [
    "stream",
    "round",
    "input_windows",
    "traditional_candidates",
    "yolo_candidates",
    "confirmed",
    "traditional_seconds",
    "raw_yolo_write_seconds",
    "total_seconds",
]


@dataclass(frozen=True)
class CropRecord:
    dataset_path: Path
    raw_source: Path
    stream: str
    time_group: str
    source_index: int
    world_tile: int
    grid_x: int
    grid_y: int

    @property
    def round_index(self) -> int:
        return self.source_index // 16


@dataclass
class MinedCandidate:
    dataset_crop: Path
    raw_source: Path
    stream: str
    time_group: str
    round_index: int
    source_index: int
    world_tile: int
    grid_x: int
    grid_y: int
    local_x: int
    local_y: int
    local_x1: int
    local_y1: int
    local_x2: int
    local_y2: int
    frame_x: int
    frame_y: int
    pano_x: int
    pano_y: int
    traditional_area: int
    traditional_score: float
    traditional_response: float
    traditional_contrast: float
    traditional_center_ring: float
    traditional_template_corr: float
    traditional_aspect: float
    traditional_compactness: float
    yolo_drone_raw: float = 0.0
    yolo_drone_weighted: float = 0.0
    yolo_bird_raw: float = 0.0
    yolo_bird_weighted: float = 0.0
    yolo_center_distance: float = 0.0
    yolo_x1: int = 0
    yolo_y1: int = 0
    yolo_x2: int = 0
    yolo_y2: int = 0


class TileMaskCache:
    def __init__(self, dataset_root: Path) -> None:
        self._mask_dir = dataset_root / "masks" / "world_tiles"
        self._full: Dict[int, np.ndarray] = {}

    def crop(self, record: CropRecord, size: int) -> np.ndarray:
        tile = record.world_tile
        full = self._full.get(tile)
        if full is None:
            path = self._mask_dir / f"tile_{tile:02d}.png"
            full = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
            if full is None:
                raise RuntimeError(f"failed to read world-tile mask: {path}")
            full = np.where(full > 0, 255, 0).astype(np.uint8)
            self._full[tile] = full
        roi = full[
            record.grid_y : record.grid_y + size,
            record.grid_x : record.grid_x + size,
        ]
        if roi.shape != (size, size):
            raise RuntimeError(
                f"invalid mask crop for {record.dataset_path}: {roi.shape}"
            )
        return roi


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", type=Path, default=DEFAULT_DATASET_ROOT)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument(
        "--streams",
        nargs="+",
        choices=("RGB", "BW"),
        default=["RGB", "BW"],
    )
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument("--input-size", type=int, default=640)
    parser.add_argument("--traditional-per-window", type=int, default=3)
    parser.add_argument("--confirm-limit-per-round", type=int, default=8)
    parser.add_argument("--confirm-nms-radius", type=int, default=256)
    parser.add_argument("--panorama-width", type=int, default=65536)
    parser.add_argument("--frame-width", type=int, default=4096)
    parser.add_argument("--frame-height", type=int, default=4096)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--dnn-threads", type=int, default=32)
    parser.add_argument("--center-radius", type=float, default=256.0)
    parser.add_argument("--min-drone-score", type=float, default=0.02)
    parser.add_argument("--drone-over-bird-ratio", type=float, default=1.10)
    parser.add_argument("--jpeg-quality", type=int, default=95)
    parser.add_argument("--round-start", type=int, default=0)
    parser.add_argument(
        "--round-end",
        type=int,
        default=-1,
        help="Inclusive final round; -1 means no upper limit.",
    )
    parser.add_argument(
        "--max-groups",
        type=int,
        default=0,
        help="Process at most this many pending stream/round groups; 0 means all.",
    )
    parser.add_argument("--progress-every", type=int, default=10)
    parser.add_argument(
        "--allow-existing-output",
        action="store_true",
        help=(
            "Allow a non-empty output directory without a group journal. "
            "Normal resumable reruns do not need this flag."
        ),
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    input_manifest = args.dataset_root / "manifest.csv"
    if not input_manifest.is_file():
        raise FileNotFoundError(f"dataset manifest not found: {input_manifest}")
    if not args.reference.is_file():
        raise FileNotFoundError(f"traditional reference not found: {args.reference}")
    if not args.model.is_file():
        raise FileNotFoundError(f"YOLO model not found: {args.model}")
    if args.crop_size <= 0 or args.input_size <= 0:
        raise ValueError("crop and input sizes must be positive")
    if args.traditional_per_window <= 0:
        raise ValueError("--traditional-per-window must be positive")
    if args.confirm_limit_per_round <= 0:
        raise ValueError("--confirm-limit-per-round must be positive")
    if args.confirm_nms_radius <= 0 or args.panorama_width <= 0:
        raise ValueError("NMS radius and panorama width must be positive")
    if args.frame_width <= 0 or args.frame_height <= 0:
        raise ValueError("frame dimensions must be positive")
    if args.workers <= 0 or args.dnn_threads <= 0:
        raise ValueError("thread counts must be positive")
    if args.center_radius <= 0:
        raise ValueError("--center-radius must be positive")
    if args.min_drone_score < 0 or args.drone_over_bird_ratio < 0:
        raise ValueError("YOLO score constraints cannot be negative")
    if not 1 <= args.jpeg_quality <= 100:
        raise ValueError("--jpeg-quality must be in [1, 100]")
    if args.round_start < 0:
        raise ValueError("--round-start cannot be negative")
    if args.round_end >= 0 and args.round_end < args.round_start:
        raise ValueError("--round-end cannot be less than --round-start")
    if args.max_groups < 0:
        raise ValueError("--max-groups cannot be negative")


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load helper module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def prepare_output(path: Path, allow_existing: bool) -> None:
    group_summary = path / "group_summary.csv"
    if path.exists() and any(path.iterdir()) and not group_summary.is_file():
        if not allow_existing:
            raise RuntimeError(
                f"non-empty output has no resume journal: {path}; "
                "use a new directory or pass --allow-existing-output"
            )
    (path / "images" / "RGB").mkdir(parents=True, exist_ok=True)
    (path / "images" / "BW").mkdir(parents=True, exist_ok=True)


def open_csv_append(path: Path, fields: Sequence[str]):
    existed = path.is_file() and path.stat().st_size > 0
    handle = path.open("a", newline="", encoding="utf-8")
    writer = csv.DictWriter(handle, fieldnames=fields)
    if not existed:
        writer.writeheader()
        handle.flush()
    return handle, writer


def load_resume_state(
    manifest_path: Path,
    group_path: Path,
) -> Tuple[Set[str], Set[Tuple[str, int]]]:
    existing_files: Set[str] = set()
    if manifest_path.is_file():
        with manifest_path.open("r", newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                value = row.get("file", "")
                if value:
                    existing_files.add(value)

    completed: Set[Tuple[str, int]] = set()
    if group_path.is_file():
        with group_path.open("r", newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                try:
                    completed.add((row["stream"], int(row["round"])))
                except (KeyError, ValueError):
                    continue
    return existing_files, completed


def row_to_crop_record(dataset_root: Path, row: Mapping[str, str]) -> CropRecord:
    return CropRecord(
        dataset_path=dataset_root / row["file"],
        raw_source=Path(row["source"]),
        stream=row["stream"],
        time_group=row["time_group"],
        source_index=int(row["source_index"]),
        world_tile=int(row["world_tile"]),
        grid_x=int(row["x"]),
        grid_y=int(row["y"]),
    )


def iter_input_groups(
    dataset_root: Path,
    streams: Set[str],
    round_start: int,
    round_end: int,
) -> Iterator[Tuple[Tuple[str, int], List[CropRecord]]]:
    path = dataset_root / "manifest.csv"
    current_key: Tuple[str, int] | None = None
    records: List[CropRecord] = []
    with path.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            stream = row["stream"]
            if stream not in streams:
                continue
            round_index = int(row["source_index"]) // 16
            if round_index < round_start:
                continue
            if round_end >= 0 and round_index > round_end:
                continue
            key = (stream, round_index)
            if current_key is not None and key != current_key:
                yield current_key, records
                records = []
            current_key = key
            records.append(row_to_crop_record(dataset_root, row))
    if current_key is not None and records:
        yield current_key, records


def traditional_candidates_for_crop(
    record: CropRecord,
    sky_mask: np.ndarray,
    traditional_module,
    template: np.ndarray,
    max_candidates: int,
    frame_width: int,
) -> List[MinedCandidate]:
    gray = cv2.imread(str(record.dataset_path), cv2.IMREAD_GRAYSCALE)
    if gray is None:
        raise RuntimeError(f"failed to decode dataset crop: {record.dataset_path}")
    if gray.shape != sky_mask.shape:
        raise RuntimeError(
            f"crop/mask size mismatch for {record.dataset_path}: "
            f"{gray.shape} vs {sky_mask.shape}"
        )
    found = traditional_module.detect_traditional_high_recall(
        record.stream,
        record.dataset_path,
        gray,
        sky_mask,
        template,
        max_candidates,
    )
    out: List[MinedCandidate] = []
    for candidate in found:
        frame_x = record.grid_x + int(candidate.x)
        frame_y = record.grid_y + int(candidate.y)
        out.append(
            MinedCandidate(
                dataset_crop=record.dataset_path,
                raw_source=record.raw_source,
                stream=record.stream,
                time_group=record.time_group,
                round_index=record.round_index,
                source_index=record.source_index,
                world_tile=record.world_tile,
                grid_x=record.grid_x,
                grid_y=record.grid_y,
                local_x=int(candidate.x),
                local_y=int(candidate.y),
                local_x1=int(candidate.x1),
                local_y1=int(candidate.y1),
                local_x2=int(candidate.x2),
                local_y2=int(candidate.y2),
                frame_x=frame_x,
                frame_y=frame_y,
                pano_x=record.world_tile * frame_width + frame_x,
                pano_y=frame_y,
                traditional_area=int(candidate.area),
                traditional_score=float(candidate.score),
                traditional_response=float(candidate.response),
                traditional_contrast=float(candidate.contrast),
                traditional_center_ring=float(candidate.center_ring),
                traditional_template_corr=float(candidate.template_corr),
                traditional_aspect=float(candidate.aspect),
                traditional_compactness=float(candidate.compactness),
            )
        )
    return out


def circular_far_enough(
    selected: Sequence[MinedCandidate],
    candidate: MinedCandidate,
    radius: int,
    panorama_width: int,
) -> bool:
    radius_squared = radius * radius
    for old in selected:
        dx = abs(candidate.pano_x - old.pano_x)
        dx = min(dx, panorama_width - dx)
        dy = candidate.pano_y - old.pano_y
        if dx * dx + dy * dy <= radius_squared:
            return False
    return True


def select_for_yolo(
    candidates: Sequence[MinedCandidate],
    limit: int,
    radius: int,
    panorama_width: int,
) -> List[MinedCandidate]:
    ranked = sorted(
        candidates,
        key=lambda candidate: candidate.traditional_score,
        reverse=True,
    )
    selected: List[MinedCandidate] = []
    for candidate in ranked:
        if not circular_far_enough(selected, candidate, radius, panorama_width):
            continue
        selected.append(candidate)
        if len(selected) >= limit:
            break
    return selected


def orient_like_dmx(image: np.ndarray) -> np.ndarray:
    return cv2.flip(cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE), 1)


def fixed_crop_color(
    image: np.ndarray,
    center_x: int,
    center_y: int,
    size: int,
) -> np.ndarray:
    out = np.zeros((size, size, 3), dtype=np.uint8)
    half = size // 2
    wanted_x = center_x - half
    wanted_y = center_y - half
    x0 = max(0, wanted_x)
    y0 = max(0, wanted_y)
    x1 = min(image.shape[1], wanted_x + size)
    y1 = min(image.shape[0], wanted_y + size)
    if x1 <= x0 or y1 <= y0:
        return out
    dx = x0 - wanted_x
    dy = y0 - wanted_y
    out[dy : dy + (y1 - y0), dx : dx + (x1 - x0)] = image[y0:y1, x0:x1]
    return out


def score_yolo(
    net: cv2.dnn.Net,
    crop: np.ndarray,
    candidate: MinedCandidate,
    preview_module,
    input_size: int,
    center_radius: float,
) -> None:
    resized = cv2.resize(crop, (input_size, input_size), interpolation=cv2.INTER_LINEAR)
    blob = cv2.dnn.blobFromImage(
        resized,
        1.0 / 255.0,
        (input_size, input_size),
        (0, 0, 0),
        swapRB=True,
        crop=False,
    )
    net.setInput(blob)
    outputs = net.forward(net.getUnconnectedOutLayersNames())
    detections = preview_module.parse_yolo_output(
        outputs[0],
        input_size,
        crop.shape[1],
    )

    center = crop.shape[1] / 2.0
    best_drone: Tuple[float, float, float, Tuple[int, int, int, int]] | None = None
    best_bird_weighted = 0.0
    best_bird_raw = 0.0
    for x1, y1, x2, y2, cls, confidence in detections:
        detection_x = (x1 + x2) * 0.5
        detection_y = (y1 + y2) * 0.5
        distance = float(np.hypot(detection_x - center, detection_y - center))
        center_weight = max(0.0, 1.0 - distance / center_radius)
        weighted = confidence * center_weight
        if cls == 0:
            if best_drone is None or weighted > best_drone[0]:
                best_drone = (weighted, confidence, distance, (x1, y1, x2, y2))
        elif cls == 1 and weighted > best_bird_weighted:
            best_bird_weighted = weighted
            best_bird_raw = confidence

    candidate.yolo_bird_weighted = best_bird_weighted
    candidate.yolo_bird_raw = best_bird_raw
    if best_drone is not None:
        weighted, raw, distance, box = best_drone
        candidate.yolo_drone_weighted = weighted
        candidate.yolo_drone_raw = raw
        candidate.yolo_center_distance = distance
        (
            candidate.yolo_x1,
            candidate.yolo_y1,
            candidate.yolo_x2,
            candidate.yolo_y2,
        ) = box


def confirmed_by_yolo(
    candidate: MinedCandidate,
    min_drone_score: float,
    drone_over_bird_ratio: float,
) -> bool:
    return (
        candidate.yolo_drone_weighted >= min_drone_score
        and candidate.yolo_drone_weighted
        >= candidate.yolo_bird_weighted * drone_over_bird_ratio
    )


def output_relative_path(candidate: MinedCandidate) -> Path:
    name = (
        f"{candidate.raw_source.stem}"
        f"_px{candidate.pano_x:05d}_py{candidate.pano_y:04d}"
        f"_fx{candidate.frame_x:04d}_fy{candidate.frame_y:04d}.jpg"
    )
    return Path("images") / candidate.stream / name


def atomic_write_jpg(
    path: Path,
    image: np.ndarray,
    quality: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(f".{path.stem}.{os.getpid()}.tmp.jpg")
    if not cv2.imwrite(
        str(temp),
        image,
        [int(cv2.IMWRITE_JPEG_QUALITY), int(quality)],
    ):
        raise RuntimeError(f"failed to write candidate JPEG: {temp}")
    os.replace(temp, path)


def manifest_row(
    candidate: MinedCandidate,
    relative: Path,
    crop_size: int,
) -> Dict[str, object]:
    frame_x1 = candidate.grid_x + candidate.local_x1
    frame_y1 = candidate.grid_y + candidate.local_y1
    frame_x2 = candidate.grid_x + candidate.local_x2
    frame_y2 = candidate.grid_y + candidate.local_y2
    wanted_x = candidate.frame_x - crop_size // 2
    wanted_y = candidate.frame_y - crop_size // 2

    def bounded(value: int) -> int:
        return max(0, min(crop_size - 1, value))

    return {
        "file": str(relative),
        "dataset_crop": str(candidate.dataset_crop),
        "raw_source": str(candidate.raw_source),
        "stream": candidate.stream,
        "time_group": candidate.time_group,
        "round": candidate.round_index,
        "source_index": candidate.source_index,
        "world_tile": candidate.world_tile,
        "grid_x": candidate.grid_x,
        "grid_y": candidate.grid_y,
        "frame_x": candidate.frame_x,
        "frame_y": candidate.frame_y,
        "pano_x": candidate.pano_x,
        "pano_y": candidate.pano_y,
        "traditional_frame_x1": frame_x1,
        "traditional_frame_y1": frame_y1,
        "traditional_frame_x2": frame_x2,
        "traditional_frame_y2": frame_y2,
        "traditional_roi_x1": bounded(frame_x1 - wanted_x),
        "traditional_roi_y1": bounded(frame_y1 - wanted_y),
        "traditional_roi_x2": bounded(frame_x2 - wanted_x),
        "traditional_roi_y2": bounded(frame_y2 - wanted_y),
        "traditional_area": candidate.traditional_area,
        "traditional_score": f"{candidate.traditional_score:.8f}",
        "traditional_response": f"{candidate.traditional_response:.8f}",
        "traditional_contrast": f"{candidate.traditional_contrast:.8f}",
        "traditional_center_ring": f"{candidate.traditional_center_ring:.8f}",
        "traditional_template_corr": f"{candidate.traditional_template_corr:.8f}",
        "traditional_aspect": f"{candidate.traditional_aspect:.8f}",
        "traditional_compactness": f"{candidate.traditional_compactness:.8f}",
        "yolo_drone_raw": f"{candidate.yolo_drone_raw:.8f}",
        "yolo_drone_weighted": f"{candidate.yolo_drone_weighted:.8f}",
        "yolo_bird_raw": f"{candidate.yolo_bird_raw:.8f}",
        "yolo_bird_weighted": f"{candidate.yolo_bird_weighted:.8f}",
        "yolo_center_distance": f"{candidate.yolo_center_distance:.4f}",
        "yolo_x1": candidate.yolo_x1,
        "yolo_y1": candidate.yolo_y1,
        "yolo_x2": candidate.yolo_x2,
        "yolo_y2": candidate.yolo_y2,
    }


def process_group(
    key: Tuple[str, int],
    records: Sequence[CropRecord],
    args: argparse.Namespace,
    executor: ThreadPoolExecutor,
    mask_cache: TileMaskCache,
    traditional_module,
    template: np.ndarray,
    preview_module,
    net: cv2.dnn.Net,
) -> Tuple[List[Dict[str, object]], Dict[str, object]]:
    group_start = time.perf_counter()
    masks = [mask_cache.crop(record, args.crop_size) for record in records]

    traditional_start = time.perf_counter()
    cv2.setNumThreads(1)

    def detect_one(pair: Tuple[CropRecord, np.ndarray]) -> List[MinedCandidate]:
        record, mask = pair
        return traditional_candidates_for_crop(
            record,
            mask,
            traditional_module,
            template,
            args.traditional_per_window,
            args.frame_width,
        )

    per_window = list(executor.map(detect_one, zip(records, masks)))
    all_traditional = [
        candidate for candidates in per_window for candidate in candidates
    ]
    yolo_candidates = select_for_yolo(
        all_traditional,
        args.confirm_limit_per_round,
        args.confirm_nms_radius,
        args.panorama_width,
    )
    traditional_seconds = time.perf_counter() - traditional_start

    yolo_start = time.perf_counter()
    cv2.setNumThreads(args.dnn_threads)
    raw_cache: Dict[Path, np.ndarray] = {}
    rows: List[Dict[str, object]] = []
    for candidate in yolo_candidates:
        oriented = raw_cache.get(candidate.raw_source)
        if oriented is None:
            raw = cv2.imread(str(candidate.raw_source), cv2.IMREAD_COLOR)
            if raw is None:
                raise RuntimeError(f"failed to decode raw source: {candidate.raw_source}")
            oriented = orient_like_dmx(raw)
            if oriented.shape[:2] != (args.frame_height, args.frame_width):
                raise RuntimeError(
                    f"unexpected oriented raw size for {candidate.raw_source}: "
                    f"{oriented.shape[1]}x{oriented.shape[0]}"
                )
            raw_cache[candidate.raw_source] = oriented
        centered = fixed_crop_color(
            oriented,
            candidate.frame_x,
            candidate.frame_y,
            args.crop_size,
        )
        score_yolo(
            net,
            centered,
            candidate,
            preview_module,
            args.input_size,
            args.center_radius,
        )
        if not confirmed_by_yolo(
            candidate,
            args.min_drone_score,
            args.drone_over_bird_ratio,
        ):
            continue
        relative = output_relative_path(candidate)
        output_path = args.out_dir / relative
        if not output_path.is_file():
            atomic_write_jpg(output_path, centered, args.jpeg_quality)
        rows.append(manifest_row(candidate, relative, args.crop_size))
    raw_cache.clear()
    yolo_seconds = time.perf_counter() - yolo_start
    cv2.setNumThreads(1)

    summary = {
        "stream": key[0],
        "round": key[1],
        "input_windows": len(records),
        "traditional_candidates": len(all_traditional),
        "yolo_candidates": len(yolo_candidates),
        "confirmed": len(rows),
        "traditional_seconds": f"{traditional_seconds:.6f}",
        "raw_yolo_write_seconds": f"{yolo_seconds:.6f}",
        "total_seconds": f"{time.perf_counter() - group_start:.6f}",
    }
    return rows, summary


def aggregate_counts(
    manifest_path: Path,
    group_path: Path,
) -> Tuple[Dict[str, int], Dict[str, int], int]:
    candidates: Dict[str, int] = {}
    if manifest_path.is_file():
        with manifest_path.open("r", newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                stream = row["stream"]
                candidates[stream] = candidates.get(stream, 0) + 1
    groups: Dict[str, int] = {}
    total_windows = 0
    if group_path.is_file():
        with group_path.open("r", newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                stream = row["stream"]
                groups[stream] = groups.get(stream, 0) + 1
                total_windows += int(row["input_windows"])
    return candidates, groups, total_windows


def write_run_summary(
    args: argparse.Namespace,
    candidates: Mapping[str, int],
    groups: Mapping[str, int],
    total_windows: int,
) -> None:
    summary = {
        "datasetRoot": str(args.dataset_root),
        "outputRoot": str(args.out_dir),
        "model": str(args.model),
        "reference": str(args.reference),
        "streams": args.streams,
        "cropSize": args.crop_size,
        "inputSize": args.input_size,
        "traditionalPerWindow": args.traditional_per_window,
        "confirmLimitPerRound": args.confirm_limit_per_round,
        "confirmNmsRadius": args.confirm_nms_radius,
        "centerRadius": args.center_radius,
        "minDroneScore": args.min_drone_score,
        "droneOverBirdRatio": args.drone_over_bird_ratio,
        "fallbackTraditionalOnEmpty": False,
        "completedGroups": dict(groups),
        "processedSkyWindows": total_windows,
        "savedCandidates": dict(candidates),
    }
    path = args.out_dir / "run_summary.json"
    # A PID-specific temporary file also permits the RGB and BW streams to be
    # processed concurrently. The final no-op resume pass rewrites one complete
    # aggregate after both workers have finished.
    temp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temp.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(temp, path)


def main() -> int:
    args = parse_args()
    validate_args(args)
    args.streams = [stream.upper() for stream in args.streams]
    prepare_output(args.out_dir, args.allow_existing_output)

    helper_dir = Path(__file__).resolve().parent
    traditional_module = load_module(
        helper_dir / "run_20260714_trad_yolo26_weights2.py",
        "dmx_20260723_full_traditional",
    )
    preview_module = load_module(
        helper_dir / "make_20260723_traditional_yolo_preview.py",
        "dmx_20260723_full_yolo",
    )
    template = traditional_module.load_template(args.reference)
    net = cv2.dnn.readNetFromONNX(str(args.model))
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

    manifest_path = args.out_dir / "manifest.csv"
    group_path = args.out_dir / "group_summary.csv"
    existing_files, completed = load_resume_state(manifest_path, group_path)
    manifest_handle, manifest_writer = open_csv_append(
        manifest_path, MANIFEST_FIELDS
    )
    group_handle, group_writer = open_csv_append(group_path, GROUP_FIELDS)
    mask_cache = TileMaskCache(args.dataset_root)

    processed_now = 0
    confirmed_now = 0
    skipped = 0
    started = time.perf_counter()
    streams = set(args.streams)
    try:
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            for key, records in iter_input_groups(
                args.dataset_root,
                streams,
                args.round_start,
                args.round_end,
            ):
                if key in completed:
                    skipped += 1
                    continue
                if args.max_groups > 0 and processed_now >= args.max_groups:
                    break
                rows, summary = process_group(
                    key,
                    records,
                    args,
                    executor,
                    mask_cache,
                    traditional_module,
                    template,
                    preview_module,
                    net,
                )
                rows_to_add = [
                    row for row in rows if str(row["file"]) not in existing_files
                ]
                if rows_to_add:
                    manifest_writer.writerows(rows_to_add)
                    manifest_handle.flush()
                    for row in rows_to_add:
                        existing_files.add(str(row["file"]))
                    confirmed_now += len(rows_to_add)
                group_writer.writerow(summary)
                group_handle.flush()
                completed.add(key)
                processed_now += 1
                if (
                    processed_now == 1
                    or (
                        args.progress_every > 0
                        and processed_now % args.progress_every == 0
                    )
                ):
                    elapsed = time.perf_counter() - started
                    rate = processed_now / max(elapsed, 1e-6)
                    print(
                        f"[progress] groups={processed_now} "
                        f"last={key[0]}:{key[1]} "
                        f"windows={summary['input_windows']} "
                        f"trad={summary['traditional_candidates']} "
                        f"yolo={summary['yolo_candidates']} "
                        f"confirmed={summary['confirmed']} "
                        f"saved_now={confirmed_now} "
                        f"rate={rate:.3f} groups/s",
                        flush=True,
                    )
    finally:
        manifest_handle.close()
        group_handle.close()
        cv2.setNumThreads(args.dnn_threads)

    candidate_counts, group_counts, total_windows = aggregate_counts(
        manifest_path, group_path
    )
    write_run_summary(
        args,
        candidate_counts,
        group_counts,
        total_windows,
    )
    print(
        f"[done] processed_now={processed_now} skipped={skipped} "
        f"total_windows={total_windows} groups={group_counts} "
        f"candidates={candidate_counts} out={args.out_dir}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "\n[stopped] interrupted; rerun the same command to resume",
            file=sys.stderr,
        )
        raise SystemExit(130)
