#!/usr/bin/env python3
"""Mine 640x640 YOLO training windows from DMX AB panoramas.

The script keeps source panoramas untouched, builds a full sliding-window index,
uses classical image processing for high-recall candidate mining, and exports
candidate/negative crops plus visual summaries for later manual labeling.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff"}
NAME_RE = re.compile(
    r"^(?P<stream>RGB|BW)_(?P<day>\d{8})_(?P<hms>\d{6})_(?P<idx>\d+)-(?P<half>[AB])$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class SourceImage:
    path: Path
    rel_path: str
    stream: str
    day: str
    hour: str
    hms: str
    index: str
    half: str


@dataclass
class WindowScore:
    x0: int
    y0: int
    score: float
    max_response: float
    components: int
    tiny_area: int
    edge_density: float
    isolatedness: float = 0.0
    texture: float = 0.0
    ground_ratio: float = 0.0
    category: str = ""
    negative_type: str = ""
    crop_file: str = ""


def parse_source(path: Path, src_root: Path) -> SourceImage | None:
    m = NAME_RE.match(path.stem)
    if not m:
        return None
    stream = m.group("stream").lower()
    day = m.group("day")
    hms = m.group("hms")
    return SourceImage(
        path=path,
        rel_path=str(path.relative_to(src_root)),
        stream=stream,
        day=day,
        hour=hms[:2],
        hms=hms,
        index=m.group("idx"),
        half=m.group("half").upper(),
    )


def discover_sources(src_root: Path, stream_filter: str) -> List[SourceImage]:
    wanted = {stream_filter.lower()} if stream_filter.lower() in {"bw", "rgb"} else {"bw", "rgb"}
    out: List[SourceImage] = []
    for path in src_root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in IMAGE_EXTS:
            continue
        item = parse_source(path, src_root)
        if item is None or item.stream not in wanted:
            continue
        out.append(item)
    out.sort(key=lambda x: (x.day, x.stream, x.hour, x.hms, int(x.index), x.half, x.rel_path))
    return out


def axis_starts(length: int, tile: int, stride: int) -> List[int]:
    if length <= tile:
        return [0]
    starts = list(range(0, length - tile + 1, stride))
    last = length - tile
    if starts[-1] != last:
        starts.append(last)
    return starts


def preprocess(gray: np.ndarray, use_morph: bool = False) -> np.ndarray:
    # Use gentle local contrast. Strong sharpening promotes leaf/building texture
    # into false "small targets"; this pass favors isolated spots on smoother sky.
    clahe = cv2.createCLAHE(clipLimit=1.4, tileGridSize=(8, 8)).apply(gray)
    smooth_small = cv2.GaussianBlur(clahe, (0, 0), 1.2)
    smooth_large = cv2.GaussianBlur(clahe, (0, 0), 6.0)

    lap = cv2.convertScaleAbs(cv2.Laplacian(smooth_small, cv2.CV_16S, ksize=3))
    dog = cv2.absdiff(
        cv2.GaussianBlur(clahe, (0, 0), 0.8),
        cv2.GaussianBlur(clahe, (0, 0), 2.8),
    )
    bright = cv2.subtract(smooth_small, smooth_large)
    dark = cv2.subtract(smooth_large, smooth_small)
    responses = [lap, dog, bright, dark]
    if use_morph:
        k9 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
        k17 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (17, 17))
        responses.extend([
            cv2.morphologyEx(clahe, cv2.MORPH_TOPHAT, k9),
            cv2.morphologyEx(clahe, cv2.MORPH_TOPHAT, k17),
            cv2.morphologyEx(clahe, cv2.MORPH_BLACKHAT, k17),
        ])
    return np.maximum.reduce(responses)


def component_stats(response: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    mean, std = cv2.meanStdDev(response)
    # Deliberately loose: high recall first, manual labeling later.
    thr = max(8.0, float(mean[0][0]) + 1.8 * float(std[0][0]))
    mask = (response >= thr).astype(np.uint8) * 255
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, np.ones((2, 2), np.uint8))
    n, labels, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    selected = np.zeros_like(mask)
    comp_points = np.zeros_like(mask)
    for i in range(1, n):
        area = int(stats[i, cv2.CC_STAT_AREA])
        if 2 <= area <= 260:
            selected[labels == i] = 255
            x = int(stats[i, cv2.CC_STAT_LEFT] + stats[i, cv2.CC_STAT_WIDTH] // 2)
            y = int(stats[i, cv2.CC_STAT_TOP] + stats[i, cv2.CC_STAT_HEIGHT] // 2)
            if 0 <= y < comp_points.shape[0] and 0 <= x < comp_points.shape[1]:
                comp_points[y, x] = 1
    return selected, comp_points


def segment_bottom_ground(img: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    """Return bottom-connected grass/ground mask and a smoothed horizon y per column."""
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
    h, w = gray.shape[:2]
    blur = cv2.GaussianBlur(gray, (0, 0), 2.0)
    lower = blur[h // 2 :, :]
    thr = max(135.0, float(np.percentile(lower, 62.0)))
    bright = (blur >= thr).astype(np.uint8) * 255
    bright[: int(h * 0.30), :] = 0

    close_k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
    open_k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    mask = cv2.morphologyEx(bright, cv2.MORPH_CLOSE, close_k, iterations=2)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, open_k, iterations=1)

    n, labels, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    bottom_connected = np.zeros_like(mask)
    bottom_labels = set(np.unique(labels[max(0, h - 3) : h, :]).tolist())
    for lab in bottom_labels:
        if lab == 0:
            continue
        if stats[lab, cv2.CC_STAT_AREA] > w * h * 0.005:
            bottom_connected[labels == lab] = 255

    boundary = np.full(w, h - 1, dtype=np.int32)
    for x in range(w):
        ys = np.where(bottom_connected[:, x] > 0)[0]
        if ys.size:
            boundary[x] = int(ys.min())

    ksize = max(15, int(w * 0.04) | 1)
    pad = ksize // 2
    padded = np.pad(boundary, (pad, pad), mode="edge")
    smooth = np.array([np.median(padded[i : i + ksize]) for i in range(w)], dtype=np.int32)
    smooth = np.clip(smooth, int(h * 0.25), h - 1)

    ground = np.zeros_like(mask)
    for x, y in enumerate(smooth):
        ground[y:, x] = 255
    return ground, smooth


def integral_sum(ii: np.ndarray, x0: int, y0: int, w: int, h: int) -> float:
    x1 = x0 + w
    y1 = y0 + h
    return float(ii[y1, x1] - ii[y0, x1] - ii[y1, x0] + ii[y0, x0])


def score_windows(
    img: np.ndarray,
    tile: int,
    stride: int,
    use_morph: bool = False,
    ground_mask: np.ndarray | None = None,
) -> Tuple[List[WindowScore], Dict[str, object]]:
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
    response = preprocess(gray, use_morph)
    mask, comp_points = component_stats(response)
    edges = cv2.Canny(gray, 40, 120)
    texture_map = cv2.convertScaleAbs(cv2.Laplacian(cv2.GaussianBlur(gray, (0, 0), 1.0), cv2.CV_16S, ksize=3))
    response_i = cv2.integral(response, sdepth=cv2.CV_64F)
    mask_i = cv2.integral((mask > 0).astype(np.uint8), sdepth=cv2.CV_64F)
    comp_i = cv2.integral(comp_points.astype(np.uint8), sdepth=cv2.CV_64F)
    edge_i = cv2.integral((edges > 0).astype(np.uint8), sdepth=cv2.CV_64F)
    texture_i = cv2.integral(texture_map, sdepth=cv2.CV_64F)
    ground_i = cv2.integral((ground_mask > 0).astype(np.uint8), sdepth=cv2.CV_64F) if ground_mask is not None else None

    xs = axis_starts(gray.shape[1], tile, stride)
    ys = axis_starts(gray.shape[0], tile, stride)
    scores: List[WindowScore] = []
    for y0 in ys:
        for x0 in xs:
            r = response[y0 : y0 + tile, x0 : x0 + tile]
            tiny_area = int(integral_sum(mask_i, x0, y0, tile, tile))
            comp_count = int(integral_sum(comp_i, x0, y0, tile, tile))
            mean_resp = integral_sum(response_i, x0, y0, tile, tile) / float(tile * tile)
            max_resp = float(r.max())
            edge_density = integral_sum(edge_i, x0, y0, tile, tile) / float(tile * tile)
            texture = integral_sum(texture_i, x0, y0, tile, tile) / float(tile * tile)
            ground_ratio = integral_sum(ground_i, x0, y0, tile, tile) / float(tile * tile) if ground_i is not None else 0.0
            isolatedness = (float(comp_count) / (1.0 + math.sqrt(max(1, tiny_area))))
            texture_penalty = edge_density * 900.0 + texture * 1.8 + max(0, comp_count - 8) * 12.0
            score = 0.36 * max_resp + 14.0 * isolatedness + 0.16 * mean_resp - texture_penalty
            scores.append(WindowScore(x0, y0, score, max_resp, comp_count, tiny_area, edge_density, isolatedness, texture, ground_ratio))

    summary = {
        "width": int(gray.shape[1]),
        "height": int(gray.shape[0]),
        "windows": len(scores),
        "response_mean": float(response.mean()),
        "response_p99": float(np.percentile(response, 99.0)),
        "response_max": float(response.max()),
        "ground_pixels": int(cv2.countNonZero(ground_mask)) if ground_mask is not None else 0,
    }
    return scores, summary


def choose_windows(
    scores: List[WindowScore],
    max_candidates: int,
    negative: int,
    max_ground_ratio: float,
    rng: random.Random,
) -> List[WindowScore]:
    valid_scores = [s for s in scores if s.ground_ratio <= max_ground_ratio]
    ranked = sorted(valid_scores, key=lambda s: s.score, reverse=True)
    if not ranked:
        return []

    # Candidate recall is aimed at small isolated objects on relatively smooth
    # backgrounds. High edge/texture windows are usually tree/building clutter.
    smooth_pool = [
        s for s in ranked
        if s.components > 0
        and s.components <= 10
        and s.tiny_area <= 1800
        and s.edge_density <= 0.055
        and s.texture <= 14.0
        and s.ground_ratio <= max_ground_ratio
    ]
    if smooth_pool:
        arr = np.array([s.score for s in smooth_pool], dtype=np.float32)
        loose_cut = max(float(np.percentile(arr, 82.0)), float(arr.mean() + 0.25 * arr.std()))
        candidate_pool = [s for s in smooth_pool if s.score >= loose_cut]
    else:
        candidate_pool = []

    candidates: List[WindowScore] = []
    for s in candidate_pool:
        if all(abs(s.x0 - k.x0) > 768 or abs(s.y0 - k.y0) > 768 for k in candidates):
            candidates.append(s)
        if len(candidates) >= max_candidates:
            break
    for s in candidates:
        s.category = "candidate"

    used = {(s.x0, s.y0) for s in candidates}

    texture_pool = [s for s in ranked if (s.x0, s.y0) not in used and (s.edge_density >= 0.07 or s.texture >= 18.0)]
    low_pool = [s for s in ranked[int(len(ranked) * 0.55) :] if (s.x0, s.y0) not in used and s.ground_ratio <= max_ground_ratio]
    neg_texture_n = negative // 2
    neg_random_n = negative - neg_texture_n
    texture_neg = rng.sample(texture_pool[: max(neg_texture_n * 8, neg_texture_n)], min(neg_texture_n, len(texture_pool))) if texture_pool else []
    for s in texture_neg:
        s.category = "negative"
        s.negative_type = "texture"
        used.add((s.x0, s.y0))
    low_pool = [s for s in low_pool if (s.x0, s.y0) not in used]
    rand = rng.sample(low_pool, min(neg_random_n, len(low_pool))) if low_pool else []
    for s in rand:
        s.category = "negative"
        s.negative_type = "random"

    return candidates + texture_neg + rand


def crop_path(out_root: Path, item: SourceImage, win: WindowScore) -> Path:
    name = (
        f"{Path(item.rel_path).stem}_x{win.x0:05d}_y{win.y0:04d}_"
        f"tile640_stride512_{win.category}.jpg"
    )
    return out_root / item.day / item.stream / item.hour / win.category / name


def save_crop(img: np.ndarray, out_path: Path, x0: int, y0: int, tile: int, quality: int) -> None:
    crop = img[y0 : y0 + tile, x0 : x0 + tile]
    if crop.shape[0] != tile or crop.shape[1] != tile:
        padded = np.zeros((tile, tile, 3), dtype=np.uint8)
        if crop.ndim == 2:
            crop = cv2.cvtColor(crop, cv2.COLOR_GRAY2BGR)
        padded[: crop.shape[0], : crop.shape[1]] = crop
        crop = padded
    out_path.parent.mkdir(parents=True, exist_ok=True)
    ok = cv2.imwrite(str(out_path), crop, [int(cv2.IMWRITE_JPEG_QUALITY), quality])
    if not ok:
        raise RuntimeError(f"write failed: {out_path}")


def map_scores_to_original(
    scores: List[WindowScore],
    scale: float,
    orig_w: int,
    orig_h: int,
    tile: int,
    stride: int,
) -> List[WindowScore]:
    if abs(scale - 1.0) < 1e-6:
        return scores
    mapped: List[WindowScore] = []
    seen = set()
    max_x = max(0, orig_w - tile)
    max_y = max(0, orig_h - tile)
    for s in scores:
        x0 = int(round((s.x0 / scale) / stride) * stride)
        y0 = int(round((s.y0 / scale) / stride) * stride)
        x0 = min(max(0, x0), max_x)
        y0 = min(max(0, y0), max_y)
        key = (x0, y0)
        if key in seen:
            continue
        seen.add(key)
        mapped.append(WindowScore(
            x0, y0, s.score, s.max_response, s.components, s.tiny_area,
            s.edge_density, s.isolatedness, s.texture, s.ground_ratio,
        ))
    return mapped


def draw_visuals(
    img: np.ndarray,
    selected: Sequence[WindowScore],
    out_dir: Path,
    stem: str,
    scale_width: int,
) -> None:
    if not selected:
        return
    h, w = img.shape[:2]
    scale = min(1.0, scale_width / float(w))
    vis = cv2.resize(img, (int(w * scale), int(h * scale)), interpolation=cv2.INTER_AREA)
    colors = {
        "candidate": (0, 0, 255),
        "negative": (255, 180, 0),
    }
    for s in selected:
        c = colors.get(s.category, (255, 255, 255))
        x0, y0 = int(s.x0 * scale), int(s.y0 * scale)
        x1, y1 = int((s.x0 + 640) * scale), int((s.y0 + 640) * scale)
        cv2.rectangle(vis, (x0, y0), (x1, y1), c, 2)
    out_dir.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out_dir / f"{stem}_boxes.jpg"), vis, [int(cv2.IMWRITE_JPEG_QUALITY), 90])


def write_csv_header(path: Path, header: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerow(header)


def append_csv_rows(path: Path, rows: Iterable[Sequence[object]]) -> None:
    with path.open("a", newline="", encoding="utf-8") as f:
        csv.writer(f).writerows(rows)


def simple_bar_chart(rows: List[Dict[str, object]], out_path: Path, title: str) -> None:
    if not rows:
        return
    labels = [str(r["label"]) for r in rows]
    values = [int(r["value"]) for r in rows]
    width = max(800, 90 * len(labels))
    height = 520
    img = np.full((height, width, 3), 255, np.uint8)
    cv2.putText(img, title, (30, 45), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (30, 30, 30), 2, cv2.LINE_AA)
    max_v = max(values) if values else 1
    chart_h = 360
    base_y = 440
    left = 60
    bar_w = max(20, (width - 120) // max(1, len(values)) - 8)
    for i, (lab, val) in enumerate(zip(labels, values)):
        x = left + i * (bar_w + 8)
        bh = int(chart_h * val / max_v) if max_v else 0
        cv2.rectangle(img, (x, base_y - bh), (x + bar_w, base_y), (80, 130, 230), -1)
        cv2.putText(img, str(val), (x, base_y - bh - 8), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (30, 30, 30), 1)
        cv2.putText(img, lab, (x, base_y + 28), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (30, 30, 30), 1)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out_path), img)


def run(args: argparse.Namespace) -> int:
    src_root = Path(args.src).resolve()
    out_root = Path(args.out).resolve()
    day = src_root.name
    run_root = out_root / day
    analysis_dir = run_root / "_analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)

    sources = discover_sources(src_root, args.stream)
    if args.limit:
        sources = sources[: args.limit]
    if not sources:
        raise SystemExit(f"no source images found under {src_root}")

    full_index = analysis_dir / "full_window_index.csv"
    sample_index = analysis_dir / "sample_manifest.csv"
    image_summary = analysis_dir / "per_image_summary.csv"
    manifest_jsonl = analysis_dir / "sample_manifest.jsonl"
    write_csv_header(full_index, [
        "source", "stream", "day", "hour", "hms", "ab", "source_index",
        "x0", "y0", "tile", "overlap", "stride", "score", "components",
        "tiny_area", "edge_density", "isolatedness", "texture", "ground_ratio",
    ])
    write_csv_header(sample_index, [
        "file", "category", "negative_type", "source", "stream", "day", "hour", "hms", "ab",
        "source_index", "x0", "y0", "tile", "overlap", "stride", "score",
        "max_response", "components", "tiny_area", "edge_density", "isolatedness", "texture", "ground_ratio",
    ])
    write_csv_header(image_summary, [
        "source", "stream", "day", "hour", "ab", "width", "height", "windows",
        "saved_candidate", "saved_negative",
        "response_mean", "response_p99", "response_max", "ground_pixels",
    ])
    manifest_jsonl.write_text("", encoding="utf-8")

    rng = random.Random(args.seed)
    totals = {"candidate": 0, "negative": 0}
    by_hour: Dict[str, int] = {}
    started = time.time()

    for idx, item in enumerate(sources, 1):
        img = cv2.imread(str(item.path), cv2.IMREAD_COLOR)
        if img is None:
            print(f"[WARN] read failed: {item.path}")
            continue
        if args.scan_scale < 0.999:
            scan_w = max(1, int(round(img.shape[1] * args.scan_scale)))
            scan_h = max(1, int(round(img.shape[0] * args.scan_scale)))
            scan_img = cv2.resize(img, (scan_w, scan_h), interpolation=cv2.INTER_AREA)
            scan_tile = max(16, int(round(args.tile * args.scan_scale)))
            scan_stride = max(8, int(round(args.stride * args.scan_scale)))
            ground_mask, _ = segment_bottom_ground(scan_img) if args.ground_filter else (None, None)
            scan_scores, scan_summary = score_windows(scan_img, scan_tile, scan_stride, args.morph, ground_mask)
            scores = map_scores_to_original(scan_scores, args.scan_scale, img.shape[1], img.shape[0], args.tile, args.stride)
            summary = {
                "width": int(img.shape[1]),
                "height": int(img.shape[0]),
                "windows": len(scores),
                "response_mean": scan_summary["response_mean"],
                "response_p99": scan_summary["response_p99"],
                "response_max": scan_summary["response_max"],
                "ground_pixels": scan_summary["ground_pixels"],
            }
        else:
            ground_mask, _ = segment_bottom_ground(img) if args.ground_filter else (None, None)
            scores, summary = score_windows(img, args.tile, args.stride, args.morph, ground_mask)
        selected = choose_windows(scores, args.max_candidates, args.negative, args.max_ground_ratio, rng)

        append_csv_rows(full_index, (
            [
                item.rel_path, item.stream, item.day, item.hour, item.hms, item.half, item.index,
                s.x0, s.y0, args.tile, args.overlap, args.stride, f"{s.score:.3f}",
                s.components, s.tiny_area, f"{s.edge_density:.6f}", f"{s.isolatedness:.6f}",
                f"{s.texture:.3f}", f"{s.ground_ratio:.6f}",
            ]
            for s in scores
        ))

        selected_rows = []
        json_lines = []
        cat_counts = {"candidate": 0, "negative": 0}
        for win in selected:
            out_path = crop_path(out_root, item, win)
            save_crop(img, out_path, win.x0, win.y0, args.tile, args.jpeg_quality)
            rel = str(out_path.relative_to(out_root))
            win.crop_file = rel
            totals[win.category] += 1
            cat_counts[win.category] += 1
            by_hour[f"{item.stream}/{item.hour}"] = by_hour.get(f"{item.stream}/{item.hour}", 0) + 1
            row = [
                rel, win.category, win.negative_type, item.rel_path, item.stream, item.day, item.hour, item.hms,
                item.half, item.index, win.x0, win.y0, args.tile, args.overlap,
                args.stride, f"{win.score:.3f}", f"{win.max_response:.3f}",
                win.components, win.tiny_area, f"{win.edge_density:.6f}",
                f"{win.isolatedness:.6f}", f"{win.texture:.3f}", f"{win.ground_ratio:.6f}",
            ]
            selected_rows.append(row)
            json_lines.append(json.dumps({
                "file": rel,
                "category": win.category,
                "negativeType": win.negative_type,
                "source": item.rel_path,
                "stream": item.stream,
                "day": item.day,
                "hour": item.hour,
                "hms": item.hms,
                "ab": item.half,
                "sourceIndex": item.index,
                "x0": win.x0,
                "y0": win.y0,
                "tile": args.tile,
                "overlap": args.overlap,
                "stride": args.stride,
                "score": round(win.score, 3),
                "maxResponse": round(win.max_response, 3),
                "components": win.components,
                "tinyArea": win.tiny_area,
                "edgeDensity": round(win.edge_density, 6),
                "isolatedness": round(win.isolatedness, 6),
                "texture": round(win.texture, 3),
                "groundRatio": round(win.ground_ratio, 6),
            }, ensure_ascii=False))
        append_csv_rows(sample_index, selected_rows)
        with manifest_jsonl.open("a", encoding="utf-8") as f:
            for line in json_lines:
                f.write(line + "\n")

        append_csv_rows(image_summary, [[
            item.rel_path, item.stream, item.day, item.hour, item.half,
            summary["width"], summary["height"], summary["windows"],
            cat_counts["candidate"], cat_counts["negative"],
            f"{summary['response_mean']:.3f}", f"{summary['response_p99']:.3f}", f"{summary['response_max']:.3f}",
            summary["ground_pixels"],
        ]])

        if idx <= args.visualize_first or (selected and args.visualize_every > 0 and idx % args.visualize_every == 0):
            draw_visuals(img, selected, analysis_dir / "visualizations" / item.stream / item.hour, item.path.stem, args.visual_width)

        if idx % args.progress_every == 0 or idx == len(sources):
            elapsed = time.time() - started
            print(
                f"[{idx}/{len(sources)}] saved candidate={totals['candidate']} "
                f"negative={totals['negative']} "
                f"elapsed={elapsed:.1f}s",
                flush=True,
            )

    summary = {
        "sourceRoot": str(src_root),
        "outputRoot": str(out_root),
        "day": day,
        "images": len(sources),
        "tile": args.tile,
        "overlap": args.overlap,
        "stride": args.stride,
        "jpegQuality": args.jpeg_quality,
        "scanScale": args.scan_scale,
        "groundFilter": args.ground_filter,
        "maxGroundRatio": args.max_ground_ratio,
        "totals": totals,
        "fullWindowIndex": str(full_index),
        "sampleManifestCsv": str(sample_index),
        "sampleManifestJsonl": str(manifest_jsonl),
        "perImageSummary": str(image_summary),
    }
    (analysis_dir / "run_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    simple_bar_chart(
        [{"label": k, "value": v} for k, v in totals.items()],
        analysis_dir / "charts" / "sample_category_counts.jpg",
        "Sample category counts",
    )
    simple_bar_chart(
        [{"label": k, "value": by_hour[k]} for k in sorted(by_hour)],
        analysis_dir / "charts" / "samples_by_stream_hour.jpg",
        "Samples by stream/hour",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Mine DMX YOLO26s 640x640 candidate windows from AB panoramas.")
    p.add_argument("--src", default="/mnt/dmx4t/data_all/20260626", help="Source day directory containing bw/rgb/hour AB panoramas.")
    p.add_argument("--out", default="/mnt/dmx4t/DMX_yangben", help="Output sample root.")
    p.add_argument("--stream", choices=["both", "bw", "rgb"], default="both")
    p.add_argument("--tile", type=int, default=640)
    p.add_argument("--overlap", type=int, default=128)
    p.add_argument("--stride", type=int, default=512)
    p.add_argument("--max-candidates", type=int, default=8, help="High-recall classical candidates saved per source image.")
    p.add_argument("--negative", type=int, default=6, help="Negative windows saved per source image; mixes texture clutter and random background.")
    p.add_argument("--jpeg-quality", type=int, default=95)
    p.add_argument("--scan-scale", type=float, default=0.125, help="Run classical mining on a downscaled image, then crop original-resolution 640 windows.")
    p.add_argument("--ground-filter", action=argparse.BooleanOptionalAction, default=True, help="Segment bottom grass/ground and skip windows with high ground coverage.")
    p.add_argument("--max-ground-ratio", type=float, default=0.12, help="Maximum grass/ground coverage allowed for saved candidate/negative windows.")
    p.add_argument("--morph", action="store_true", help="Also run morphology TopHat/BlackHat preprocessing. Slower but sometimes more sensitive.")
    p.add_argument("--seed", type=int, default=20260626)
    p.add_argument("--limit", type=int, default=0, help="Debug limit on number of source images.")
    p.add_argument("--visualize-first", type=int, default=12)
    p.add_argument("--visualize-every", type=int, default=0, help="Generate periodic visualization every N images; 0 disables periodic visualization.")
    p.add_argument("--visual-width", type=int, default=2400)
    p.add_argument("--progress-every", type=int, default=20)
    return p


if __name__ == "__main__":
    raise SystemExit(run(build_parser().parse_args()))
