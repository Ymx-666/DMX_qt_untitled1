#!/usr/bin/env python3
"""Run traditional candidates + YOLOv26 weights2 on one 20260714 AB panorama."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


SOURCE_DIR = Path("/mnt/dmx4t/data/recordings/20260714/bw/17")
OUT_DIR = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/tradition+yolo26")
REFERENCE = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg")
MODEL = Path("/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx")
NAME_RE = re.compile(r"^(BW_\d{8}_\d{6}_\d+)-([AB])\.jpg$")


@dataclass
class Candidate:
    stream: str
    source: Path
    x: int
    y: int
    x1: int
    y1: int
    x2: int
    y2: int
    area: int
    score: float
    response: float
    contrast: float
    center_ring: float
    template_corr: float
    aspect: float
    compactness: float
    yolo_drone: float = 0.0
    yolo_bird: float = 0.0
    yolo_cls: str = ""
    yolo_x1: int = 0
    yolo_y1: int = 0
    yolo_x2: int = 0
    yolo_y2: int = 0
    final_score: float = 0.0


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load_sky_module():
    return load_module(Path(__file__).resolve().parent / "make_20260714_temporal_sky_mask.py", "dmx_temporal_sky_mask2")


def list_complete_pairs(source_dir: Path) -> List[Tuple[str, Path, Path]]:
    grouped: Dict[str, Dict[str, Path]] = {}
    for path in source_dir.glob("*.jpg"):
        match = NAME_RE.match(path.name)
        if not match:
            continue
        grouped.setdefault(match.group(1), {})[match.group(2)] = path
    pairs: List[Tuple[str, Path, Path]] = []
    for stem in sorted(grouped):
        item = grouped[stem]
        if "A" in item and "B" in item:
            pairs.append((stem, item["A"], item["B"]))
    return pairs


def load_gray(path: Path) -> np.ndarray:
    gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if gray is None:
        raise RuntimeError(f"failed to read image: {path}")
    return gray


def fixed_crop_gray(gray: np.ndarray, cx: int, cy: int, size: int, fill: int = 0) -> np.ndarray:
    out = np.full((size, size), fill, dtype=gray.dtype)
    half = size // 2
    wanted_x = cx - half
    wanted_y = cy - half
    x0 = max(0, wanted_x)
    y0 = max(0, wanted_y)
    x1 = min(gray.shape[1], wanted_x + size)
    y1 = min(gray.shape[0], wanted_y + size)
    if x1 <= x0 or y1 <= y0:
        return out
    dx = x0 - wanted_x
    dy = y0 - wanted_y
    out[dy : dy + (y1 - y0), dx : dx + (x1 - x0)] = gray[y0:y1, x0:x1]
    return out


def normalized_highpass(patch: np.ndarray) -> np.ndarray:
    eq = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(patch.astype(np.uint8, copy=False))
    hp = eq.astype(np.float32) - cv2.GaussianBlur(eq, (0, 0), 7.0).astype(np.float32)
    hp -= float(hp.mean())
    norm = float(np.linalg.norm(hp))
    if norm > 1e-6:
        hp /= norm
    return hp


def load_template(reference: Path, size: int = 96) -> np.ndarray:
    ref = load_gray(reference)
    patch = fixed_crop_gray(ref, ref.shape[1] // 2, ref.shape[0] // 2, size, int(np.mean(ref)))
    return normalized_highpass(patch)


def template_corr(gray: np.ndarray, templ: np.ndarray, cx: int, cy: int, fill: int) -> float:
    patch = fixed_crop_gray(gray, cx, cy, templ.shape[1], fill)
    hp = normalized_highpass(patch)
    return float(np.sum(hp * templ))


def point_features(gray: np.ndarray, response: np.ndarray, cx: int, cy: int, fill: int, size: int = 96) -> Tuple[float, float]:
    patch = fixed_crop_gray(gray, cx, cy, size, fill)
    resp = fixed_crop_gray(response, cx, cy, size, 0).astype(np.float32)
    yy, xx = np.mgrid[0:size, 0:size]
    dx = xx - size / 2.0
    dy = yy - size / 2.0
    inner = ((dx / 13.0) ** 2 + (dy / 8.0) ** 2) <= 1.0
    outer = ((dx / 36.0) ** 2 + (dy / 24.0) ** 2) <= 1.0
    ring = outer & ~inner
    inner_mean = float(np.mean(patch[inner]))
    ring_mean = float(np.mean(patch[ring]))
    resp_mean = float(np.mean(resp[inner]))
    contrast = ring_mean - inner_mean
    return contrast, resp_mean * 2.0 + contrast


def far_enough(kept: Sequence[Candidate], cand: Candidate, radius: int) -> bool:
    r2 = radius * radius
    for old in kept:
        dx = old.x - cand.x
        dy = old.y - cand.y
        if dx * dx + dy * dy <= r2:
            return False
    return True


def detect_traditional_high_recall(stream: str,
                                   source: Path,
                                   gray_in: np.ndarray,
                                   sky_mask: np.ndarray,
                                   templ: np.ndarray,
                                   max_candidates: int) -> List[Candidate]:
    gray = cv2.medianBlur(gray_in, 3)
    fill = int(np.mean(gray))
    edge_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    detection_mask = cv2.erode(sky_mask, edge_kernel, iterations=1)
    if cv2.countNonZero(detection_mask) <= max(1024, cv2.countNonZero(sky_mask) // 5):
        detection_mask = sky_mask

    local_bg = cv2.GaussianBlur(gray, (0, 0), 9.0)
    dark_response = cv2.subtract(local_bg, gray)
    eq = cv2.createCLAHE(clipLimit=1.8, tileGridSize=(8, 8)).apply(gray)
    blur = cv2.GaussianBlur(eq, (0, 0), 1.2)
    sharp = cv2.addWeighted(eq, 1.45, blur, -0.45, 0.0)
    blackhat = cv2.morphologyEx(sharp, cv2.MORPH_BLACKHAT, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (15, 15)))
    response = np.maximum(dark_response, blackhat)
    response = cv2.bitwise_and(response, detection_mask)

    mean, std = cv2.meanStdDev(response, mask=detection_mask)
    thr = float(mean[0][0]) + 2.4 * float(std[0][0])
    thr = min(max(thr, 8.0), 80.0)
    mask = (response >= thr).astype(np.uint8) * 255
    mask = cv2.bitwise_and(mask, detection_mask)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3)))

    count, labels, stats, centroids = cv2.connectedComponentsWithStats(mask, 8)
    pool: List[Candidate] = []
    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        if area < 4 or area > 650:
            continue
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        if ww <= 0 or hh <= 0 or ww > 128 or hh > 128:
            continue
        aspect = ww / float(max(1, hh))
        if aspect < 0.28 or aspect > 3.8:
            continue
        compactness = area / float(max(1, ww * hh))
        if compactness < 0.14:
            continue
        cx = int(round(float(centroids[lab][0])))
        cy = int(round(float(centroids[lab][1])))
        if not (0 <= cx < gray.shape[1] and 0 <= cy < gray.shape[0]):
            continue
        if detection_mask[cy, cx] == 0:
            continue
        comp = labels[y : y + hh, x : x + ww] == lab
        if float(np.mean(detection_mask[y : y + hh, x : x + ww][comp])) < 205.0:
            continue
        response_max = float(np.max(response[y : y + hh, x : x + ww][comp]))
        contrast, center_ring = point_features(gray, response, cx, cy, fill)
        corr = template_corr(gray, templ, cx, cy, fill)
        line_penalty = max(aspect, 1.0 / max(0.001, aspect))
        if contrast < 2.5 and center_ring < 9.0 and corr < 0.05:
            continue
        score = response_max + contrast * 1.5 + center_ring * 1.4 + max(0.0, corr) * 115.0 + compactness * 12.0
        if line_penalty > 2.4:
            score *= 0.78
        if score < 35.0:
            continue
        pool.append(Candidate(
            stream=stream,
            source=source,
            x=cx,
            y=cy,
            x1=x,
            y1=y,
            x2=x + ww,
            y2=y + hh,
            area=area,
            score=score,
            response=response_max,
            contrast=contrast,
            center_ring=center_ring,
            template_corr=corr,
            aspect=aspect,
            compactness=compactness,
        ))

    pool.sort(key=lambda item: item.score, reverse=True)
    kept: List[Candidate] = []
    for cand in pool:
        if not far_enough(kept, cand, 44):
            continue
        kept.append(cand)
        if len(kept) >= max_candidates:
            break
    return kept


def parse_yolo_outputs(outs: Sequence[np.ndarray], input_size: int, crop_size: int, min_score: float = 1e-6) -> List[Tuple[int, int, int, int, int, float]]:
    detections: List[Tuple[int, int, int, int, int, float]] = []

    def parse_row(row: np.ndarray) -> None:
        attrs = int(row.shape[0])
        if attrs < 5:
            return
        scores = row[4:]
        cls = int(np.argmax(scores))
        conf = float(scores[cls])
        if conf < min_score:
            return
        cx, cy, bw, bh = [float(v) for v in row[:4]]
        if max(abs(cx), abs(cy), abs(bw), abs(bh)) <= 2.0:
            cx *= input_size
            cy *= input_size
            bw *= input_size
            bh *= input_size
        if bw <= 1.0 or bh <= 1.0:
            return
        scale = float(crop_size) / float(input_size)
        x1 = int(round((cx - bw * 0.5) * scale))
        y1 = int(round((cy - bh * 0.5) * scale))
        x2 = int(round((cx + bw * 0.5) * scale))
        y2 = int(round((cy + bh * 0.5) * scale))
        x1 = max(0, min(crop_size - 1, x1))
        y1 = max(0, min(crop_size - 1, y1))
        x2 = max(0, min(crop_size - 1, x2))
        y2 = max(0, min(crop_size - 1, y2))
        if x2 > x1 and y2 > y1:
            detections.append((x1, y1, x2, y2, cls, conf))

    for out in outs:
        arr = np.asarray(out, dtype=np.float32)
        if arr.ndim == 3:
            arr = arr[0]
        if arr.ndim != 2:
            continue
        if arr.shape[0] <= 256 and arr.shape[1] > arr.shape[0]:
            for i in range(arr.shape[1]):
                parse_row(arr[:, i])
        else:
            for i in range(arr.shape[0]):
                parse_row(arr[i, :])
    return detections


def run_yolo_for_candidate(net: cv2.dnn.Net,
                           gray: np.ndarray,
                           cand: Candidate,
                           crop_size: int,
                           input_size: int) -> None:
    crop = fixed_crop_gray(gray, cand.x, cand.y, crop_size, 0)
    crop_bgr = cv2.cvtColor(crop, cv2.COLOR_GRAY2BGR)
    resized = cv2.resize(crop_bgr, (input_size, input_size), interpolation=cv2.INTER_LINEAR)
    blob = cv2.dnn.blobFromImage(resized, 1.0 / 255.0, (input_size, input_size), (0, 0, 0), True, False)
    net.setInput(blob)
    outs = net.forward(net.getUnconnectedOutLayersNames())
    detections = parse_yolo_outputs(outs, input_size, crop_size, 1e-6)

    center = crop_size / 2.0
    best_drone = 0.0
    best_bird = 0.0
    best_det: Tuple[int, int, int, int, int, float] | None = None
    for det in detections:
        x1, y1, x2, y2, cls, conf = det
        dcx = (x1 + x2) * 0.5
        dcy = (y1 + y2) * 0.5
        dist = float(np.hypot(dcx - center, dcy - center))
        center_weight = max(0.0, 1.0 - dist / 160.0)
        weighted = conf * center_weight
        if cls == 0 and weighted > best_drone:
            best_drone = weighted
            best_det = det
        if cls == 1 and weighted > best_bird:
            best_bird = weighted
            if best_det is None:
                best_det = det
    cand.yolo_drone = best_drone
    cand.yolo_bird = best_bird
    if best_det is not None:
        x1, y1, x2, y2, cls, _ = best_det
        gx0 = cand.x - crop_size // 2
        gy0 = cand.y - crop_size // 2
        cand.yolo_x1 = max(0, gx0 + x1)
        cand.yolo_y1 = max(0, gy0 + y1)
        cand.yolo_x2 = max(0, gx0 + x2)
        cand.yolo_y2 = max(0, gy0 + y2)
        cand.yolo_cls = "drone" if cls == 0 else "bird"


def select_yolo_filter(candidates: Sequence[Candidate], keep_per_stream: int) -> List[Candidate]:
    if not candidates:
        return []
    max_score = max(c.yolo_drone for c in candidates)
    if max_score <= 0:
        return []
    thr = max(0.01, min(0.25, max_score * 0.45))
    if max_score < 0.01:
        thr = max_score * 0.55
    selected = [c for c in candidates if c.yolo_drone >= thr and c.yolo_drone >= c.yolo_bird * 1.15]
    selected.sort(key=lambda c: (c.yolo_drone, c.score), reverse=True)
    return nms_candidates(selected, 96)[:keep_per_stream]


def select_fusion(candidates: Sequence[Candidate], keep_per_stream: int, yolo_weight: float) -> List[Candidate]:
    if not candidates:
        return []
    trad_scale = max(1.0, float(np.percentile([c.score for c in candidates], 95)))
    yolo_scale = max(1e-6, float(np.percentile([c.yolo_drone for c in candidates], 95)))
    bird_scale = max(1e-6, float(np.percentile([c.yolo_bird for c in candidates], 95)))
    for c in candidates:
        trad_norm = min(1.0, c.score / trad_scale)
        yolo_norm = min(1.0, c.yolo_drone / yolo_scale)
        bird_norm = min(1.0, c.yolo_bird / bird_scale)
        corr_bonus = max(0.0, min(1.0, c.template_corr)) * 0.10
        c.final_score = (1.0 - yolo_weight) * trad_norm + yolo_weight * yolo_norm + corr_bonus - 0.18 * bird_norm
    ranked = sorted(candidates, key=lambda c: c.final_score, reverse=True)
    ranked = [c for c in ranked if c.final_score >= max(0.18, ranked[0].final_score * 0.35)]
    return nms_candidates(ranked, 96)[:keep_per_stream]


def nms_candidates(candidates: Sequence[Candidate], radius: int) -> List[Candidate]:
    kept: List[Candidate] = []
    r2 = radius * radius
    for cand in candidates:
        ok = True
        for old in kept:
            dx = cand.x - old.x
            dy = cand.y - old.y
            if dx * dx + dy * dy <= r2:
                ok = False
                break
        if ok:
            kept.append(cand)
    return kept


def draw_boxes(gray: np.ndarray, boxes: Sequence[Candidate], mode: str) -> np.ndarray:
    out = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    thickness = 5 if gray.shape[1] > 10000 else 2
    font_scale = 1.0 if gray.shape[1] > 10000 else 0.55
    for idx, c in enumerate(boxes, 1):
        if c.yolo_x2 > c.yolo_x1 and c.yolo_y2 > c.yolo_y1:
            x1, y1, x2, y2 = c.yolo_x1, c.yolo_y1, c.yolo_x2, c.yolo_y2
        else:
            pad = 16
            x1, y1, x2, y2 = c.x1 - pad, c.y1 - pad, c.x2 + pad, c.y2 + pad
        x1 = max(0, min(out.shape[1] - 1, x1))
        y1 = max(0, min(out.shape[0] - 1, y1))
        x2 = max(0, min(out.shape[1] - 1, x2))
        y2 = max(0, min(out.shape[0] - 1, y2))
        cv2.rectangle(out, (x1, y1), (x2, y2), (0, 0, 255), thickness)
        if mode == "fusion":
            label = f"#{idx} F{c.final_score:.2f} Y{c.yolo_drone:.3g}"
        else:
            label = f"#{idx} Y{c.yolo_drone:.3g}"
        cv2.putText(out, label, (max(0, x1), max(40, y1 - 12)), cv2.FONT_HERSHEY_SIMPLEX,
                    font_scale, (0, 0, 255), max(1, thickness // 2), cv2.LINE_AA)
    return out


def save_ab_preview(paths: Sequence[Path], out_path: Path, width: int = 4096) -> None:
    panels: List[np.ndarray] = []
    for path in paths:
        img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if img is None:
            continue
        scale = width / float(img.shape[1])
        panels.append(cv2.resize(img, (width, max(1, int(img.shape[0] * scale))), interpolation=cv2.INTER_AREA))
    if panels:
        cv2.imwrite(str(out_path), np.vstack(panels), [int(cv2.IMWRITE_JPEG_QUALITY), 94])


def write_candidates(path: Path, candidates: Sequence[Candidate]) -> None:
    fields = [
        "stream", "source", "x", "y", "x1", "y1", "x2", "y2", "area", "score",
        "response", "contrast", "center_ring", "template_corr", "aspect", "compactness",
        "yolo_drone", "yolo_bird", "yolo_cls", "yolo_x1", "yolo_y1", "yolo_x2", "yolo_y2",
        "final_score",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for c in candidates:
            writer.writerow({field: getattr(c, field) for field in fields})


def clean_out(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    (path / "01_yolo_filter").mkdir(parents=True, exist_ok=True)
    (path / "02_fusion_score").mkdir(parents=True, exist_ok=True)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=SOURCE_DIR)
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR)
    parser.add_argument("--reference", type=Path, default=REFERENCE)
    parser.add_argument("--model", type=Path, default=MODEL)
    parser.add_argument("--pairs-for-mask", type=int, default=3)
    parser.add_argument("--input-size", type=int, default=640)
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument("--max-traditional-per-stream", type=int, default=160)
    parser.add_argument("--keep-per-stream", type=int, default=8)
    parser.add_argument("--yolo-weight", type=float, default=0.72)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if not args.model.is_file():
        raise RuntimeError(f"model not found: {args.model}")
    if not args.reference.is_file():
        raise RuntimeError(f"reference not found: {args.reference}")
    pairs = list_complete_pairs(args.source_dir)
    if len(pairs) < args.pairs_for_mask:
        raise RuntimeError(f"not enough complete pairs in {args.source_dir}")
    stem, a_path, b_path = pairs[0]
    mask_pairs = pairs[: args.pairs_for_mask]
    clean_out(args.out_dir)

    sky = load_sky_module()
    templ = load_template(args.reference)
    net = cv2.dnn.readNetFromONNX(str(args.model))
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

    all_candidates: List[Candidate] = []
    source_by_stream = {"A": a_path, "B": b_path}
    image_by_stream: Dict[str, np.ndarray] = {}
    mask_by_stream: Dict[str, np.ndarray] = {}

    for stream in ("A", "B"):
        paths = [pair[1] if stream == "A" else pair[2] for pair in mask_pairs]
        mask = sky.build_stream_mask(stream, paths).sure
        gray = load_gray(paths[0])
        mask_by_stream[stream] = mask
        image_by_stream[stream] = gray
        cands = detect_traditional_high_recall(
            stream, source_by_stream[stream], gray, mask, templ, args.max_traditional_per_stream)
        print(f"{stream}: traditional candidates={len(cands)}", flush=True)
        for idx, cand in enumerate(cands, 1):
            run_yolo_for_candidate(net, gray, cand, args.crop_size, args.input_size)
            if idx % 25 == 0:
                print(f"{stream}: yolo scored {idx}/{len(cands)}", flush=True)
        all_candidates.extend(cands)

    write_candidates(args.out_dir / "all_traditional_yolo_scored_candidates.csv", all_candidates)

    filter_paths: List[Path] = []
    fusion_paths: List[Path] = []
    final_filter: List[Candidate] = []
    final_fusion: List[Candidate] = []
    for stream in ("A", "B"):
        stream_cands = [c for c in all_candidates if c.stream == stream]
        filter_boxes = select_yolo_filter(stream_cands, args.keep_per_stream)
        fusion_boxes = select_fusion(stream_cands, args.keep_per_stream, args.yolo_weight)
        final_filter.extend(filter_boxes)
        final_fusion.extend(fusion_boxes)

        filter_img = draw_boxes(image_by_stream[stream], filter_boxes, "filter")
        fusion_img = draw_boxes(image_by_stream[stream], fusion_boxes, "fusion")
        filter_path = args.out_dir / "01_yolo_filter" / f"{stem}-{stream}_final_boxes_full.jpg"
        fusion_path = args.out_dir / "02_fusion_score" / f"{stem}-{stream}_final_boxes_full.jpg"
        cv2.imwrite(str(filter_path), filter_img, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
        cv2.imwrite(str(fusion_path), fusion_img, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
        filter_paths.append(filter_path)
        fusion_paths.append(fusion_path)

    write_candidates(args.out_dir / "01_yolo_filter" / "final_boxes.csv", final_filter)
    write_candidates(args.out_dir / "02_fusion_score" / "final_boxes.csv", final_fusion)
    save_ab_preview(filter_paths, args.out_dir / "01_yolo_filter" / f"{stem}_AB_final_boxes.jpg")
    save_ab_preview(fusion_paths, args.out_dir / "02_fusion_score" / f"{stem}_AB_final_boxes.jpg")

    print(f"source_pair={stem}")
    print(f"out_dir={args.out_dir}")
    print(f"yolo_filter_boxes={len(final_filter)}")
    print(f"fusion_boxes={len(final_fusion)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
