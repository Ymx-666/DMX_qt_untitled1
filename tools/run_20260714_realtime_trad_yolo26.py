#!/usr/bin/env python3
"""Realtime-constrained traditional candidate + YOLO hard-filter test.

This script models the online path after sky masks and the YOLO network are
already initialized. It processes one complete AB panorama using:

1. cached v1 sky masks,
2. cheap traditional pre-ranking,
3. expensive traditional features only on a small shortlist,
4. batched YOLO confirmation.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


SOURCE_DIR = Path("/mnt/dmx4t/data/recordings/20260714/bw/17")
OUT_DIR = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/tradition+yolo26/realtime_6s")
REFERENCE = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg")
MODEL = Path("/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx")
MASK_A = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1/first_A_v1_detect_sky_mask.png")
MASK_B = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1/first_B_v1_detect_sky_mask.png")
NAME_RE = re.compile(r"^(BW_\d{8}_\d{6}_\d+)-([AB])\.jpg$")


@dataclass
class Candidate:
    stream: str
    source: str
    x: int
    y: int
    x1: int
    y1: int
    x2: int
    y2: int
    area: int
    cheap_score: float
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


def tic() -> float:
    return time.perf_counter()


def toc(t0: float) -> float:
    return time.perf_counter() - t0


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


def load_mask(path: Path, shape: Tuple[int, int] | None = None) -> np.ndarray:
    mask = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if mask is None:
        raise RuntimeError(f"failed to read mask: {path}")
    if shape is not None and mask.shape != shape:
        mask = cv2.resize(mask, (shape[1], shape[0]), interpolation=cv2.INTER_NEAREST)
    _, mask = cv2.threshold(mask, 0, 255, cv2.THRESH_BINARY)
    return mask


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
    contrast = float(np.mean(patch[ring]) - np.mean(patch[inner]))
    center_ring = float(np.mean(resp[inner]) * 2.0 + contrast)
    return contrast, center_ring


def far_enough_xy(points: Sequence[Tuple[int, int]], x: int, y: int, radius: int) -> bool:
    r2 = radius * radius
    for px, py in points:
        dx = px - x
        dy = py - y
        if dx * dx + dy * dy <= r2:
            return False
    return True


def spatial_nms_select(candidates: Sequence[Candidate], limit: int, radius: int) -> List[Candidate]:
    selected: List[Candidate] = []
    centers: List[Tuple[int, int]] = []
    for cand in candidates:
        if far_enough_xy(centers, cand.x, cand.y, radius):
            selected.append(cand)
            centers.append((cand.x, cand.y))
            if len(selected) >= limit:
                break
    return selected


def build_response(gray_in: np.ndarray, sky_mask: np.ndarray) -> Tuple[np.ndarray, np.ndarray, Dict[str, float]]:
    times: Dict[str, float] = {}
    t = tic()
    gray = cv2.medianBlur(gray_in, 3)
    times["median"] = toc(t)

    t = tic()
    edge_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    detection_mask = cv2.erode(sky_mask, edge_kernel, iterations=1)
    if cv2.countNonZero(detection_mask) <= max(1024, cv2.countNonZero(sky_mask) // 5):
        detection_mask = sky_mask
    times["mask_erode"] = toc(t)

    t = tic()
    local_bg = cv2.GaussianBlur(gray, (0, 0), 9.0)
    dark_response = cv2.subtract(local_bg, gray)
    times["local_dark"] = toc(t)

    t = tic()
    # Keep the same contrast model as the higher-recall offline version. The
    # runtime speedup comes from limiting expensive candidate features, not from
    # removing CLAHE; removing it can drop the real point target from the pool.
    eq = cv2.createCLAHE(clipLimit=1.8, tileGridSize=(8, 8)).apply(gray)
    blur = cv2.GaussianBlur(eq, (0, 0), 1.2)
    sharp = cv2.addWeighted(eq, 1.45, blur, -0.45, 0.0)
    blackhat = cv2.morphologyEx(
        sharp, cv2.MORPH_BLACKHAT,
        cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (15, 15)))
    response = np.maximum(dark_response, blackhat)
    response = cv2.bitwise_and(response, detection_mask)
    times["clahe_blackhat_response"] = toc(t)
    return gray, response, detection_mask, times


def detect_traditional_realtime(stream: str,
                                source: Path,
                                gray_in: np.ndarray,
                                sky_mask: np.ndarray,
                                templ: np.ndarray,
                                feature_limit: int,
                                yolo_limit: int) -> Tuple[List[Candidate], Dict[str, float], Dict[str, int]]:
    total_t = tic()
    gray, response, detection_mask, sub_times = build_response(gray_in, sky_mask)
    fill = int(np.mean(gray))

    t = tic()
    mean, std = cv2.meanStdDev(response, mask=detection_mask)
    thr = float(mean[0][0]) + 3.0 * float(std[0][0])
    thr = min(max(thr, 10.0), 88.0)
    binary = (response >= thr).astype(np.uint8) * 255
    binary = cv2.bitwise_and(binary, detection_mask)
    binary = cv2.morphologyEx(binary, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3)))
    count, labels, stats, centroids = cv2.connectedComponentsWithStats(binary, 8)
    sub_times["threshold_cc"] = toc(t)

    t = tic()
    cheap_pool: List[Candidate] = []
    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        if area < 4 or area > 520:
            continue
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        if ww <= 0 or hh <= 0 or ww > 110 or hh > 110:
            continue
        aspect = ww / float(max(1, hh))
        if aspect < 0.35 or aspect > 3.1:
            continue
        compactness = area / float(max(1, ww * hh))
        if compactness < 0.20:
            continue
        cx = int(round(float(centroids[lab][0])))
        cy = int(round(float(centroids[lab][1])))
        if not (0 <= cx < gray.shape[1] and 0 <= cy < gray.shape[0]):
            continue
        if detection_mask[cy, cx] == 0:
            continue
        # Cheap score deliberately avoids per-component boolean masks. Lower
        # sky-edge-like bands are deweighted, not removed, because they often
        # contain tree/building highlights but may still contain valid targets.
        roi = response[y : y + hh, x : x + ww]
        response_max = float(np.max(roi)) if roi.size else 0.0
        line_penalty = max(aspect, 1.0 / max(0.001, aspect))
        raw_cheap = response_max + compactness * 18.0 + min(area, 160) * 0.025 - max(0.0, line_penalty - 1.8) * 4.0
        lower_band = max(0.0, cy - gray.shape[0] * 0.46) / max(1.0, gray.shape[0] * 0.25)
        lower_penalty = min(42.0, lower_band * 42.0)
        cheap = raw_cheap - lower_penalty
        if raw_cheap < 14.0:
            continue
        cheap_pool.append(Candidate(
            stream=stream,
            source=str(source),
            x=cx,
            y=cy,
            x1=x,
            y1=y,
            x2=x + ww,
            y2=y + hh,
            area=area,
            cheap_score=cheap,
            score=0.0,
            response=response_max,
            contrast=0.0,
            center_ring=0.0,
            template_corr=0.0,
            aspect=aspect,
            compactness=compactness,
        ))
    cheap_pool.sort(key=lambda item: item.cheap_score, reverse=True)
    shortlist = spatial_nms_select(cheap_pool, feature_limit, 56)
    kept = spatial_nms_select(cheap_pool, yolo_limit, 256)
    selected_ids = {id(cand) for cand in shortlist}
    feature_targets = list(shortlist)
    for cand in kept:
        if id(cand) not in selected_ids:
            feature_targets.append(cand)
            selected_ids.add(id(cand))
    sub_times["cheap_rank"] = toc(t)

    t = tic()
    feature_pool: List[Candidate] = []
    for cand in feature_targets:
        contrast, center_ring = point_features(gray, response, cand.x, cand.y, fill)
        corr = template_corr(gray, templ, cand.x, cand.y, fill)
        line_penalty = max(cand.aspect, 1.0 / max(0.001, cand.aspect))
        score = cand.response + contrast * 1.35 + center_ring * 1.25 + max(0.0, corr) * 105.0 + cand.compactness * 13.0
        if line_penalty > 2.3:
            score *= 0.78
        cand.contrast = contrast
        cand.center_ring = center_ring
        cand.template_corr = corr
        cand.score = max(score, cand.cheap_score)
        if not (contrast < 1.0 and center_ring < 7.0 and corr < 0.03) and score >= 32.0:
            feature_pool.append(cand)
    feature_pool.sort(key=lambda item: item.score, reverse=True)
    sub_times["feature_rank"] = toc(t)

    t = tic()
    sub_times["traditional_nms"] = toc(t)
    sub_times["traditional_total"] = toc(total_t)
    stats_out = {
        "components": count - 1,
        "cheap_pool": len(cheap_pool),
        "feature_shortlist": len(shortlist),
        "feature_pool": len(feature_pool),
        "yolo_candidates": len(kept),
        "feature_spatial_radius": 56,
        "yolo_spatial_radius": 256,
        "threshold": int(round(thr)),
    }
    return kept, sub_times, stats_out


def parse_yolo_sample(sample: np.ndarray, input_size: int, crop_size: int, min_score: float = 1e-4) -> List[Tuple[int, int, int, int, int, float]]:
    if sample.ndim != 2:
        return []
    if sample.shape[0] <= 256 and sample.shape[1] > sample.shape[0]:
        rows = sample.T
    else:
        rows = sample
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
    if not np.any(valid_size):
        return []
    boxes = boxes[valid_size]
    classes = classes[valid_size]
    confidences = confidences[valid_size]

    scale = float(crop_size) / float(input_size)
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
            x1[valid_box], y1[valid_box], x2[valid_box], y2[valid_box],
            classes[valid_box], confidences[valid_box])
    ]


def assign_best_yolo(cand: Candidate,
                     detections: Sequence[Tuple[int, int, int, int, int, float]],
                     crop_size: int,
                     center_radius: float = 256.0) -> None:
    center = crop_size / 2.0
    best_det: Tuple[int, int, int, int, int, float] | None = None
    best_drone = 0.0
    best_bird = 0.0
    for det in detections:
        x1, y1, x2, y2, cls, conf = det
        dcx = (x1 + x2) * 0.5
        dcy = (y1 + y2) * 0.5
        dist = float(np.hypot(dcx - center, dcy - center))
        center_weight = max(0.0, 1.0 - dist / center_radius)
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


def run_yolo_batch(net: cv2.dnn.Net,
                   images_by_stream: Dict[str, np.ndarray],
                   candidates: Sequence[Candidate],
                   crop_size: int,
                   input_size: int,
                   batch_size: int) -> Dict[str, float]:
    times: Dict[str, float] = {}
    if not candidates:
        return {"yolo_total": 0.0, "yolo_batches": 0.0, "yolo_candidates": 0.0}
    total_t = tic()
    batches = 0
    effective_batch = max(1, batch_size)
    batch_failed = False
    for start in range(0, len(candidates), effective_batch):
        batch = candidates[start : start + effective_batch]
        t = tic()
        crops = []
        for cand in batch:
            gray = images_by_stream[cand.stream]
            crop = fixed_crop_gray(gray, cand.x, cand.y, crop_size, 0)
            crop_bgr = cv2.cvtColor(crop, cv2.COLOR_GRAY2BGR)
            resized = cv2.resize(crop_bgr, (input_size, input_size), interpolation=cv2.INTER_LINEAR)
            crops.append(resized)
        blob = cv2.dnn.blobFromImages(crops, 1.0 / 255.0, (input_size, input_size), (0, 0, 0), True, False)
        times["yolo_preprocess"] = times.get("yolo_preprocess", 0.0) + toc(t)

        t = tic()
        net.setInput(blob)
        try:
            outs = net.forward(net.getUnconnectedOutLayersNames())
        except cv2.error:
            if effective_batch == 1:
                raise
            batch_failed = True
            break
        times["yolo_forward"] = times.get("yolo_forward", 0.0) + toc(t)

        t = tic()
        out = np.asarray(outs[0], dtype=np.float32) if isinstance(outs, (list, tuple)) else np.asarray(outs, dtype=np.float32)
        if out.ndim == 3 and out.shape[0] == len(batch):
            samples = [out[i] for i in range(out.shape[0])]
        elif out.ndim == 3 and len(batch) == 1:
            samples = [out[0]]
        else:
            # Fallback: split unsupported shapes conservatively.
            samples = [out[0] if out.ndim == 3 else out for _ in batch]
        for cand, sample in zip(batch, samples):
            assign_best_yolo(cand, parse_yolo_sample(sample, input_size, crop_size), crop_size)
        times["yolo_parse"] = times.get("yolo_parse", 0.0) + toc(t)
        batches += 1
    if batch_failed:
        return run_yolo_batch(net, images_by_stream, candidates, crop_size, input_size, 1)
    times["yolo_total"] = toc(total_t)
    times["yolo_batches"] = float(batches)
    times["yolo_candidates"] = float(len(candidates))
    return times


def nms_candidates(candidates: Sequence[Candidate], radius: int) -> List[Candidate]:
    kept: List[Candidate] = []
    centers: List[Tuple[int, int]] = []
    for cand in candidates:
        if far_enough_xy(centers, cand.x, cand.y, radius):
            kept.append(cand)
            centers.append((cand.x, cand.y))
    return kept


def select_yolo_filter(candidates: Sequence[Candidate], keep_per_stream: int) -> List[Candidate]:
    if not candidates:
        return []
    max_score = max(c.yolo_drone for c in candidates)
    if max_score <= 0.0:
        return []
    thr = max(0.003, min(0.20, max_score * 0.45))
    selected = [c for c in candidates if c.yolo_drone >= thr and c.yolo_drone >= c.yolo_bird * 1.10]
    selected.sort(key=lambda item: (item.yolo_drone, item.score), reverse=True)
    return nms_candidates(selected, 96)[:keep_per_stream]


def draw_boxes(gray: np.ndarray, boxes: Sequence[Candidate]) -> np.ndarray:
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
        label = f"#{idx} Y{c.yolo_drone:.3g}"
        cv2.putText(out, label, (max(0, x1), max(40, y1 - 12)), cv2.FONT_HERSHEY_SIMPLEX,
                    font_scale, (0, 0, 255), max(1, thickness // 2), cv2.LINE_AA)
    return out


def save_ab_preview(paths: Sequence[Path], out_path: Path, width: int = 4096) -> None:
    panels = []
    for path in paths:
        img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if img is None:
            continue
        scale = width / float(img.shape[1])
        panels.append(cv2.resize(img, (width, max(1, int(img.shape[0] * scale))), interpolation=cv2.INTER_AREA))
    if panels:
        cv2.imwrite(str(out_path), np.vstack(panels), [int(cv2.IMWRITE_JPEG_QUALITY), 94])


def write_candidates(path: Path, candidates: Sequence[Candidate]) -> None:
    fields = list(asdict(candidates[0]).keys()) if candidates else list(Candidate(
        "", "", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0). __dict__.keys())
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for cand in candidates:
            writer.writerow(asdict(cand))


def write_timing_csv(path: Path, rows: Sequence[Dict[str, object]]) -> None:
    fields = ["case", "stage", "seconds"]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def make_timing_chart(path: Path, rows: Sequence[Dict[str, object]]) -> None:
    labels = [str(r["stage"]) for r in rows if r["case"] == "optimized_online"]
    values = [float(r["seconds"]) for r in rows if r["case"] == "optimized_online"]
    width = 1300
    height = 620
    img = np.full((height, width, 3), 255, np.uint8)
    cv2.putText(img, "Optimized online AB timing (seconds)", (30, 48),
                cv2.FONT_HERSHEY_SIMPLEX, 1.1, (30, 30, 30), 2, cv2.LINE_AA)
    max_v = max(values + [1.0])
    left = 210
    top = 90
    row_h = 58
    bar_w = width - left - 180
    for i, (label, value) in enumerate(zip(labels, values)):
        y = top + i * row_h
        cv2.putText(img, label, (20, y + 28), cv2.FONT_HERSHEY_SIMPLEX, 0.68, (40, 40, 40), 1, cv2.LINE_AA)
        w = int(bar_w * value / max_v)
        cv2.rectangle(img, (left, y), (left + w, y + 32), (70, 120, 220), -1)
        cv2.putText(img, f"{value:.3f}s", (left + w + 12, y + 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, (20, 20, 20), 1, cv2.LINE_AA)
    cv2.imwrite(str(path), img)


def clean_out(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=SOURCE_DIR)
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR)
    parser.add_argument("--reference", type=Path, default=REFERENCE)
    parser.add_argument("--model", type=Path, default=MODEL)
    parser.add_argument("--mask-a", type=Path, default=MASK_A)
    parser.add_argument("--mask-b", type=Path, default=MASK_B)
    parser.add_argument("--input-size", type=int, default=640)
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument("--feature-limit", type=int, default=120)
    parser.add_argument("--yolo-limit-per-stream", type=int, default=8)
    parser.add_argument("--keep-per-stream", type=int, default=8)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--skip-full-output", action="store_true", help="Skip full-resolution result image writing for realtime timing.")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    clean_out(args.out_dir)
    pairs = list_complete_pairs(args.source_dir)
    if not pairs:
        raise RuntimeError(f"no AB pairs found: {args.source_dir}")
    stem, a_path, b_path = pairs[0]

    init_t = tic()
    templ = load_template(args.reference)
    net = cv2.dnn.readNetFromONNX(str(args.model))
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
    init_seconds = toc(init_t)

    online_t = tic()
    load_t = tic()
    images = {"A": load_gray(a_path), "B": load_gray(b_path)}
    masks = {
        "A": load_mask(args.mask_a, images["A"].shape),
        "B": load_mask(args.mask_b, images["B"].shape),
    }
    load_seconds = toc(load_t)

    all_candidates: List[Candidate] = []
    per_stream_stats: Dict[str, Dict[str, object]] = {}
    timing_rows: List[Dict[str, object]] = [
        {"case": "baseline_measured", "stage": "sky_mask", "seconds": 6.896},
        {"case": "baseline_measured", "stage": "traditional", "seconds": 18.596},
        {"case": "baseline_measured", "stage": "yolo_320_single", "seconds": 22.766},
        {"case": "baseline_measured", "stage": "draw", "seconds": 0.150},
    ]

    traditional_total = 0.0
    for stream, path in (("A", a_path), ("B", b_path)):
        cands, times, stats_out = detect_traditional_realtime(
            stream, path, images[stream], masks[stream], templ,
            args.feature_limit, args.yolo_limit_per_stream)
        traditional_total += times["traditional_total"]
        per_stream_stats[stream] = {**stats_out, **times}
        all_candidates.extend(cands)
        print(f"{stream}: components={stats_out['components']} cheap={stats_out['cheap_pool']} "
              f"shortlist={stats_out['feature_shortlist']} yolo_candidates={len(cands)} "
              f"time={times['traditional_total']:.3f}s", flush=True)

    yolo_times = run_yolo_batch(net, images, all_candidates, args.crop_size, args.input_size, args.batch_size)

    select_t = tic()
    final_boxes: List[Candidate] = []
    for stream in ("A", "B"):
        stream_cands = [c for c in all_candidates if c.stream == stream]
        final_boxes.extend(select_yolo_filter(stream_cands, args.keep_per_stream))
    select_seconds = toc(select_t)

    draw_t = tic()
    full_paths: List[Path] = []
    if not args.skip_full_output:
        for stream in ("A", "B"):
            img = draw_boxes(images[stream], [c for c in final_boxes if c.stream == stream])
            out_path = args.out_dir / f"{stem}-{stream}_realtime_final_boxes_full.jpg"
            cv2.imwrite(str(out_path), img, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
            full_paths.append(out_path)
        save_ab_preview(full_paths, args.out_dir / f"{stem}_AB_realtime_final_boxes.jpg")
    draw_seconds = toc(draw_t)
    online_seconds = toc(online_t)
    realtime_core_seconds = load_seconds + traditional_total + yolo_times["yolo_total"] + select_seconds

    timing_rows.extend([
        {"case": "optimized_online", "stage": "load_images_masks", "seconds": load_seconds},
        {"case": "optimized_online", "stage": "traditional_limited", "seconds": traditional_total},
        {"case": "optimized_online", "stage": "yolo_batch", "seconds": yolo_times["yolo_total"]},
        {"case": "optimized_online", "stage": "select", "seconds": select_seconds},
        {"case": "optimized_online", "stage": "draw_write", "seconds": draw_seconds},
        {"case": "optimized_online", "stage": "core_no_visual_export", "seconds": realtime_core_seconds},
        {"case": "optimized_online", "stage": "total", "seconds": online_seconds},
    ])

    write_candidates(args.out_dir / "traditional_limited_yolo_scored_candidates.csv", all_candidates)
    write_candidates(args.out_dir / "final_boxes.csv", final_boxes)
    write_timing_csv(args.out_dir / "timing_comparison.csv", timing_rows)
    make_timing_chart(args.out_dir / "timing_chart.jpg", timing_rows)

    summary = {
        "source_pair": stem,
        "model": str(args.model),
        "reference": str(args.reference),
        "mask_a": str(args.mask_a),
        "mask_b": str(args.mask_b),
        "init_seconds_not_online": init_seconds,
        "online_seconds": online_seconds,
        "realtime_core_seconds": realtime_core_seconds,
        "load_seconds": load_seconds,
        "traditional_total_seconds": traditional_total,
        "yolo_total_seconds": yolo_times["yolo_total"],
        "draw_seconds": draw_seconds,
        "traditional_candidates_for_yolo": len(all_candidates),
        "final_boxes": len(final_boxes),
        "feature_limit": args.feature_limit,
        "yolo_limit_per_stream": args.yolo_limit_per_stream,
        "batch_size": args.batch_size,
        "skip_full_output": args.skip_full_output,
        "per_stream": per_stream_stats,
        "yolo_times": yolo_times,
    }
    (args.out_dir / "run_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
