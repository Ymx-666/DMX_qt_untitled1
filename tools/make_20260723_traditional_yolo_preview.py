#!/usr/bin/env python3
"""Select a small 20260723 traditional-candidate + YOLO confirmation preview.

The input manifest is produced by the DMX realtime traditional detector while
replaying the same 20260723 raw frames.  Candidate windows are already
512x512 and centered on the traditional candidate.  This script independently
runs the configured YOLOv26s weights2 ONNX model, applies the same center
weighting used by the confirm path, removes duplicate replay outputs, and
exports the highest scoring candidates for human review.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


DEFAULT_MANIFEST = Path(
    "/mnt/dmx4t/data/dmx_test/candidates/20260723/15/manifest.jsonl"
)
DEFAULT_MODEL = Path(
    "/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx"
)
DEFAULT_OUT_DIR = Path(
    "/mnt/dmx4t/DMX_yangben/20260723/traditional_yolo_preview_5"
)


@dataclass
class ScoredCandidate:
    source_path: Path
    source_name: str
    file_index: int
    pano_x: int
    pano_y: int
    frame_x: int
    frame_y: int
    traditional_score: float
    detector: str
    traditional_x1: int
    traditional_y1: int
    traditional_x2: int
    traditional_y2: int
    yolo_drone_raw: float = 0.0
    yolo_drone_weighted: float = 0.0
    yolo_bird_raw: float = 0.0
    yolo_bird_weighted: float = 0.0
    yolo_x1: int = 0
    yolo_y1: int = 0
    yolo_x2: int = 0
    yolo_y2: int = 0
    yolo_center_distance: float = 0.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument(
        "--traditional-pool-limit",
        type=int,
        default=300,
        help="YOLO-score this many highest traditional candidates after deduplication.",
    )
    parser.add_argument("--input-size", type=int, default=640)
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument("--center-radius", type=float, default=256.0)
    parser.add_argument("--min-drone-score", type=float, default=0.02)
    parser.add_argument("--drone-over-bird-ratio", type=float, default=1.10)
    parser.add_argument(
        "--allow-existing-output",
        action="store_true",
        help="Replace the contents of an existing preview output directory.",
    )
    parser.add_argument("--progress-every", type=int, default=25)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if not args.manifest.is_file():
        raise FileNotFoundError(f"traditional manifest not found: {args.manifest}")
    if not args.model.is_file():
        raise FileNotFoundError(f"YOLO model not found: {args.model}")
    if args.top_k <= 0:
        raise ValueError("--top-k must be positive")
    if args.traditional_pool_limit < args.top_k:
        raise ValueError("--traditional-pool-limit must be at least --top-k")
    if args.input_size <= 0 or args.crop_size <= 0:
        raise ValueError("input and crop sizes must be positive")
    if args.center_radius <= 0:
        raise ValueError("--center-radius must be positive")
    if args.min_drone_score < 0:
        raise ValueError("--min-drone-score cannot be negative")
    if args.drone_over_bird_ratio < 0:
        raise ValueError("--drone-over-bird-ratio cannot be negative")


def prepare_output(path: Path, allow_existing: bool) -> None:
    if path.exists() and any(path.iterdir()):
        if not allow_existing:
            raise RuntimeError(
                f"output directory is not empty: {path}; "
                "pass --allow-existing-output to replace it"
            )
        shutil.rmtree(path)
    (path / "images").mkdir(parents=True, exist_ok=True)
    (path / "annotated").mkdir(parents=True, exist_ok=True)


def load_traditional_pool(
    manifest_path: Path,
    pool_limit: int,
) -> List[ScoredCandidate]:
    by_path: Dict[str, ScoredCandidate] = {}
    with manifest_path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"invalid JSON at {manifest_path}:{line_number}: {exc}"
                ) from exc
            source_path = Path(str(item.get("path", "")))
            if not source_path.is_file():
                continue
            file_index = int(item.get("fileIdx", 0))
            pano_x = int(item.get("panoX", 0))
            pano_y = int(item.get("panoY", 0))
            candidate = ScoredCandidate(
                source_path=source_path,
                source_name=str(item.get("source", source_path.name)),
                file_index=file_index,
                pano_x=pano_x,
                pano_y=pano_y,
                frame_x=int(item.get("frameX", 0)),
                frame_y=int(item.get("frameY", 0)),
                traditional_score=float(item.get("score", 0.0)),
                detector=str(item.get("detector", "")),
                traditional_x1=int(item.get("roiBoxX1", 0)),
                traditional_y1=int(item.get("roiBoxY1", 0)),
                traditional_x2=int(item.get("roiBoxX2", 0)),
                traditional_y2=int(item.get("roiBoxY2", 0)),
            )
            # The replay was run more than once. Suffixed files can share the
            # same reported panorama coordinate while containing a different
            # cached frame, so only collapse repeated manifest rows that point
            # to the exact same saved JPEG. Source-frame deduplication happens
            # after YOLO scoring, when the best visual candidate is known.
            key = str(source_path)
            old = by_path.get(key)
            if old is None or candidate.traditional_score > old.traditional_score:
                by_path[key] = candidate
    ranked = sorted(
        by_path.values(),
        key=lambda candidate: candidate.traditional_score,
        reverse=True,
    )
    return ranked[:pool_limit]


def parse_yolo_output(
    output: np.ndarray,
    input_size: int,
    crop_size: int,
    min_score: float = 1e-8,
) -> List[Tuple[int, int, int, int, int, float]]:
    sample = np.asarray(output, dtype=np.float32)
    if sample.ndim == 3:
        sample = sample[0]
    if sample.ndim != 2:
        return []
    rows = sample.T if sample.shape[0] <= 256 and sample.shape[1] > sample.shape[0] else sample
    if rows.shape[1] < 5:
        return []

    boxes = rows[:, :4].astype(np.float32, copy=True)
    scores = rows[:, 4:]
    classes = np.argmax(scores, axis=1).astype(np.int32)
    confidences = scores[np.arange(scores.shape[0]), classes]
    keep = confidences >= min_score
    if not np.any(keep):
        return []

    boxes = boxes[keep]
    classes = classes[keep]
    confidences = confidences[keep]
    normalized = np.max(np.abs(boxes), axis=1) <= 2.0
    boxes[normalized] *= float(input_size)

    valid_size = (boxes[:, 2] > 1.0) & (boxes[:, 3] > 1.0)
    boxes = boxes[valid_size]
    classes = classes[valid_size]
    confidences = confidences[valid_size]
    if boxes.size == 0:
        return []

    scale = crop_size / float(input_size)
    x1 = np.rint((boxes[:, 0] - boxes[:, 2] * 0.5) * scale).astype(np.int32)
    y1 = np.rint((boxes[:, 1] - boxes[:, 3] * 0.5) * scale).astype(np.int32)
    x2 = np.rint((boxes[:, 0] + boxes[:, 2] * 0.5) * scale).astype(np.int32)
    y2 = np.rint((boxes[:, 1] + boxes[:, 3] * 0.5) * scale).astype(np.int32)
    x1 = np.clip(x1, 0, crop_size - 1)
    y1 = np.clip(y1, 0, crop_size - 1)
    x2 = np.clip(x2, 0, crop_size - 1)
    y2 = np.clip(y2, 0, crop_size - 1)
    valid_box = (x2 > x1) & (y2 > y1)
    return [
        (int(a), int(b), int(c), int(d), int(cls), float(conf))
        for a, b, c, d, cls, conf in zip(
            x1[valid_box],
            y1[valid_box],
            x2[valid_box],
            y2[valid_box],
            classes[valid_box],
            confidences[valid_box],
        )
    ]


def score_with_yolo(
    net: cv2.dnn.Net,
    candidate: ScoredCandidate,
    input_size: int,
    crop_size: int,
    center_radius: float,
) -> None:
    image = cv2.imread(str(candidate.source_path), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"failed to decode candidate: {candidate.source_path}")
    if image.shape[1] != crop_size or image.shape[0] != crop_size:
        raise RuntimeError(
            f"unexpected candidate size {image.shape[1]}x{image.shape[0]}: "
            f"{candidate.source_path}"
        )

    resized = cv2.resize(image, (input_size, input_size), interpolation=cv2.INTER_LINEAR)
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
    detections = parse_yolo_output(outputs[0], input_size, crop_size)

    center = crop_size / 2.0
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
        weighted, confidence, distance, box = best_drone
        candidate.yolo_drone_weighted = weighted
        candidate.yolo_drone_raw = confidence
        candidate.yolo_center_distance = distance
        (
            candidate.yolo_x1,
            candidate.yolo_y1,
            candidate.yolo_x2,
            candidate.yolo_y2,
        ) = box


def select_final(
    candidates: Sequence[ScoredCandidate],
    top_k: int,
    min_drone_score: float,
    drone_over_bird_ratio: float,
) -> List[ScoredCandidate]:
    confirmed = [
        candidate
        for candidate in candidates
        if candidate.yolo_drone_weighted >= min_drone_score
        and candidate.yolo_drone_weighted
        >= candidate.yolo_bird_weighted * drone_over_bird_ratio
    ]
    confirmed.sort(
        key=lambda candidate: (
            candidate.yolo_drone_weighted,
            candidate.traditional_score,
        ),
        reverse=True,
    )

    # Replay experiments may have written the same frame more than once.
    # Keep at most one window per original source frame in the preview.
    selected: List[ScoredCandidate] = []
    used_sources = set()
    for candidate in confirmed:
        source_key = (candidate.file_index, candidate.source_name)
        if source_key in used_sources:
            continue
        used_sources.add(source_key)
        selected.append(candidate)
        if len(selected) >= top_k:
            break
    return selected


def candidate_row(
    rank: int,
    candidate: ScoredCandidate,
    clean_path: Path | None = None,
    annotated_path: Path | None = None,
) -> Dict[str, object]:
    return {
        "rank": rank,
        "clean_file": str(clean_path) if clean_path else "",
        "annotated_file": str(annotated_path) if annotated_path else "",
        "traditional_candidate": str(candidate.source_path),
        "source": candidate.source_name,
        "file_index": candidate.file_index,
        "pano_x": candidate.pano_x,
        "pano_y": candidate.pano_y,
        "frame_x": candidate.frame_x,
        "frame_y": candidate.frame_y,
        "traditional_score": f"{candidate.traditional_score:.6f}",
        "yolo_drone_raw": f"{candidate.yolo_drone_raw:.8f}",
        "yolo_drone_weighted": f"{candidate.yolo_drone_weighted:.8f}",
        "yolo_bird_raw": f"{candidate.yolo_bird_raw:.8f}",
        "yolo_bird_weighted": f"{candidate.yolo_bird_weighted:.8f}",
        "yolo_center_distance": f"{candidate.yolo_center_distance:.4f}",
        "yolo_x1": candidate.yolo_x1,
        "yolo_y1": candidate.yolo_y1,
        "yolo_x2": candidate.yolo_x2,
        "yolo_y2": candidate.yolo_y2,
        "traditional_x1": candidate.traditional_x1,
        "traditional_y1": candidate.traditional_y1,
        "traditional_x2": candidate.traditional_x2,
        "traditional_y2": candidate.traditional_y2,
        "detector": candidate.detector,
    }


def write_csv(path: Path, rows: Iterable[Dict[str, object]]) -> None:
    rows = list(rows)
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def draw_annotated(image: np.ndarray, candidate: ScoredCandidate, rank: int) -> np.ndarray:
    annotated = image.copy()
    if candidate.traditional_x2 > candidate.traditional_x1:
        cv2.rectangle(
            annotated,
            (candidate.traditional_x1, candidate.traditional_y1),
            (candidate.traditional_x2, candidate.traditional_y2),
            (0, 220, 255),
            1,
        )
    if candidate.yolo_x2 > candidate.yolo_x1:
        cv2.rectangle(
            annotated,
            (candidate.yolo_x1, candidate.yolo_y1),
            (candidate.yolo_x2, candidate.yolo_y2),
            (0, 0, 255),
            2,
        )
    label = (
        f"#{rank} drone={candidate.yolo_drone_weighted:.3f} "
        f"bird={candidate.yolo_bird_weighted:.3f} "
        f"trad={candidate.traditional_score:.1f}"
    )
    cv2.rectangle(annotated, (0, 0), (511, 34), (20, 20, 20), -1)
    cv2.putText(
        annotated,
        label,
        (8, 24),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.52,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )
    return annotated


def export_final(
    out_dir: Path,
    candidates: Sequence[ScoredCandidate],
) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    annotated_images: List[np.ndarray] = []
    for rank, candidate in enumerate(candidates, 1):
        clean_relative = Path("images") / (
            f"{rank:02d}_{candidate.source_path.name}"
        )
        annotated_relative = Path("annotated") / (
            f"{rank:02d}_{candidate.source_path.name}"
        )
        clean_path = out_dir / clean_relative
        annotated_path = out_dir / annotated_relative
        shutil.copy2(candidate.source_path, clean_path)
        image = cv2.imread(str(candidate.source_path), cv2.IMREAD_COLOR)
        if image is None:
            raise RuntimeError(f"failed to read selected image: {candidate.source_path}")
        annotated = draw_annotated(image, candidate, rank)
        if not cv2.imwrite(
            str(annotated_path),
            annotated,
            [int(cv2.IMWRITE_JPEG_QUALITY), 95],
        ):
            raise RuntimeError(f"failed to write annotated preview: {annotated_path}")
        annotated_images.append(annotated)
        rows.append(
            candidate_row(
                rank,
                candidate,
                clean_path=clean_relative,
                annotated_path=annotated_relative,
            )
        )

    if annotated_images:
        sheet = np.hstack(annotated_images)
        if not cv2.imwrite(
            str(out_dir / "contact_sheet_annotated.jpg"),
            sheet,
            [int(cv2.IMWRITE_JPEG_QUALITY), 95],
        ):
            raise RuntimeError("failed to write contact sheet")
    return rows


def write_summary(
    path: Path,
    args: argparse.Namespace,
    pool_size: int,
    confirmed_size: int,
    selected_size: int,
) -> None:
    summary = {
        "traditionalManifest": str(args.manifest),
        "model": str(args.model),
        "classNames": ["drone", "bird"],
        "traditionalPoolScored": pool_size,
        "confirmedBeforeSourceDedup": confirmed_size,
        "selectedPreview": selected_size,
        "topK": args.top_k,
        "inputSize": args.input_size,
        "cropSize": args.crop_size,
        "centerRadius": args.center_radius,
        "minDroneScore": args.min_drone_score,
        "droneOverBirdRatio": args.drone_over_bird_ratio,
        "boxLegend": {
            "red": "YOLO drone box",
            "yellow": "traditional connected-component box",
        },
    }
    temp_path = path.with_suffix(".json.tmp")
    with temp_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(temp_path, path)


def main() -> int:
    args = parse_args()
    validate_args(args)
    prepare_output(args.out_dir, args.allow_existing_output)

    pool = load_traditional_pool(args.manifest, args.traditional_pool_limit)
    print(f"[traditional] unique_pool_to_score={len(pool)}", flush=True)
    net = cv2.dnn.readNetFromONNX(str(args.model))
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
    for index, candidate in enumerate(pool, 1):
        score_with_yolo(
            net,
            candidate,
            args.input_size,
            args.crop_size,
            args.center_radius,
        )
        if (
            index == 1
            or index == len(pool)
            or (args.progress_every > 0 and index % args.progress_every == 0)
        ):
            print(f"[yolo] {index}/{len(pool)}", flush=True)

    all_rows = [
        candidate_row(index, candidate)
        for index, candidate in enumerate(
            sorted(
                pool,
                key=lambda item: (
                    item.yolo_drone_weighted,
                    item.traditional_score,
                ),
                reverse=True,
            ),
            1,
        )
    ]
    write_csv(args.out_dir / "scored_traditional_candidates.csv", all_rows)

    confirmed = [
        candidate
        for candidate in pool
        if candidate.yolo_drone_weighted >= args.min_drone_score
        and candidate.yolo_drone_weighted
        >= candidate.yolo_bird_weighted * args.drone_over_bird_ratio
    ]
    selected = select_final(
        pool,
        args.top_k,
        args.min_drone_score,
        args.drone_over_bird_ratio,
    )
    if len(selected) < args.top_k:
        raise RuntimeError(
            f"only found {len(selected)} confirmed unique-source candidates; "
            f"requested {args.top_k}"
        )

    final_rows = export_final(args.out_dir, selected)
    write_csv(args.out_dir / "manifest.csv", final_rows)
    write_summary(
        args.out_dir / "run_summary.json",
        args,
        pool_size=len(pool),
        confirmed_size=len(confirmed),
        selected_size=len(selected),
    )
    print(
        f"[done] confirmed={len(confirmed)} selected={len(selected)} "
        f"out={args.out_dir}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
