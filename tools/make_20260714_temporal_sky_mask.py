#!/usr/bin/env python3
"""Build a stricter sky mask for the first 20260714 AB panorama.

The mask is computed from the first three complete AB panorama pairs. A and B
are processed independently, then stacked only for visual review.

Outputs go to:

    /mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1

The overlay uses:

    cyan   = clear sky
    blue   = cloud sky
    yellow = maybe edge/review area
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import cv2
import numpy as np


SOURCE_DIR = Path("/mnt/dmx4t/data/recordings/20260714/bw/17")
OUT_DIR = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/sky—mask/v1")
PAIR_COUNT = 3
SCALE = 0.25


@dataclass
class StreamMask:
    stream: str
    first_image: Path
    sure: np.ndarray
    clear: np.ndarray
    cloud: np.ndarray
    maybe: np.ndarray
    candidate_first: np.ndarray
    temporal_support: np.ndarray
    temporal_std_small: np.ndarray
    stats: Dict[str, float]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=SOURCE_DIR)
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR)
    parser.add_argument("--pairs", type=int, default=PAIR_COUNT)
    parser.add_argument("--ab-width", type=int, default=8192)
    parser.add_argument("--clean", action="store_true", default=True)
    return parser.parse_args()


def clean_output_dir(out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for path in out_dir.iterdir():
        if path.is_file() or path.is_symlink():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)


def list_complete_pairs(source_dir: Path, count: int) -> List[Tuple[str, Path, Path]]:
    stems: Dict[str, Dict[str, Path]] = {}
    pattern = re.compile(r"^(BW_\d{8}_\d{6}_\d+)-([AB])\.jpg$")
    for path in source_dir.glob("*.jpg"):
        match = pattern.match(path.name)
        if not match:
            continue
        stem, stream = match.group(1), match.group(2)
        stems.setdefault(stem, {})[stream] = path
    pairs: List[Tuple[str, Path, Path]] = []
    for stem in sorted(stems):
        item = stems[stem]
        if "A" in item and "B" in item:
            pairs.append((stem, item["A"], item["B"]))
        if len(pairs) >= count:
            break
    if len(pairs) < count:
        raise RuntimeError(f"only found {len(pairs)} complete AB pairs in {source_dir}")
    return pairs


def load_gray(path: Path) -> np.ndarray:
    gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if gray is None:
        raise RuntimeError(f"failed to read image: {path}")
    return gray


def small_gray(path: Path) -> np.ndarray:
    gray = load_gray(path)
    return cv2.resize(gray, None, fx=SCALE, fy=SCALE, interpolation=cv2.INTER_AREA)


def local_features(small: np.ndarray) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    blur = cv2.GaussianBlur(small, (0, 0), 3.0)
    grad_x = cv2.Sobel(blur, cv2.CV_32F, 1, 0, 3)
    grad_y = cv2.Sobel(blur, cv2.CV_32F, 0, 1, 3)
    grad = cv2.magnitude(grad_x, grad_y)
    lap = np.abs(cv2.Laplacian(blur, cv2.CV_32F, ksize=3))
    mean = cv2.blur(blur.astype(np.float32), (17, 17))
    sq = cv2.blur(blur.astype(np.float32) ** 2, (17, 17))
    texture = np.sqrt(np.maximum(sq - mean ** 2, 0.0))
    return blur, grad, texture, lap


def thresholds_for(first: np.ndarray) -> Dict[str, float]:
    blur, grad, texture, lap = local_features(first)
    h = first.shape[0]
    upper = slice(0, max(1, int(h * 0.60)))
    bright_seed = min(115.0, max(72.0, float(np.percentile(blur[upper, :], 78))))
    bright_candidate = min(70.0, max(48.0, bright_seed - 28.0))
    cloud_bright = max(42.0, 58.0 - max(0.0, bright_seed - 85.0) * 0.65)
    cloud_grad = min(42.0, max(20.0, float(np.percentile(grad[upper, :], 86))))
    cloud_texture = min(24.0, max(8.0, float(np.percentile(texture[upper, :], 86))))
    cloud_lap = min(36.0, max(12.0, float(np.percentile(lap[upper, :], 88))))
    return {
        "bright_candidate": bright_candidate,
        "bright_seed": bright_seed,
        "cloud_bright": cloud_bright,
        "grad_candidate": min(18.0, max(7.0, float(np.percentile(grad[upper, :], 62)))),
        "grad_seed": min(8.0, max(3.0, float(np.percentile(grad[upper, :], 42)))),
        "cloud_grad": cloud_grad,
        "texture_candidate": min(8.0, max(2.8, float(np.percentile(texture[upper, :], 66)))),
        "texture_seed": min(4.5, max(1.2, float(np.percentile(texture[upper, :], 45)))),
        "cloud_texture": cloud_texture,
        "lap_candidate": min(18.0, max(5.0, float(np.percentile(lap[upper, :], 72)))),
        "cloud_lap": cloud_lap,
        "cloud_edge_density": 0.22,
    }


def frame_candidate_and_seed(small: np.ndarray, th: Dict[str, float]) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    blur, grad, texture, lap = local_features(small)
    h, w = small.shape
    yy = np.arange(h, dtype=np.float32)[:, None]
    upper_limit = yy < h * 0.76

    candidate = (
        (blur.astype(np.float32) >= th["bright_candidate"])
        & (grad <= th["grad_candidate"])
        & (texture <= th["texture_candidate"])
        & (lap <= th["lap_candidate"])
        & upper_limit
    ).astype(np.uint8)

    seed = (
        (blur.astype(np.float32) >= th["bright_seed"])
        & (grad <= th["grad_seed"])
        & (texture <= th["texture_seed"])
        & (yy < h * 0.62)
    ).astype(np.uint8)

    # Cloud sky branch: allow slow, large-scale gray variation, but reject
    # fragmented high-frequency leaf/building texture by local edge density.
    high_edge = (
        (grad > max(18.0, th["cloud_grad"] * 0.85))
        | (lap > max(8.0, th["cloud_lap"] * 0.65))
    ).astype(np.float32)
    edge_density = cv2.blur(high_edge, (17, 17))
    cloud = (
        (blur.astype(np.float32) >= th["cloud_bright"])
        & (grad <= th["cloud_grad"])
        & (texture <= th["cloud_texture"])
        & (lap <= th["cloud_lap"])
        & (edge_density <= th["cloud_edge_density"])
        & (yy < h * 0.78)
    ).astype(np.uint8)

    open_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    close_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    cloud_close_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (13, 13))
    candidate = cv2.morphologyEx(candidate * 255, cv2.MORPH_OPEN, open_kernel)
    candidate = cv2.morphologyEx(candidate, cv2.MORPH_CLOSE, close_kernel)
    seed = cv2.morphologyEx(seed * 255, cv2.MORPH_OPEN, open_kernel)
    cloud = cv2.morphologyEx(cloud * 255, cv2.MORPH_OPEN, open_kernel)
    cloud = cv2.morphologyEx(cloud, cv2.MORPH_CLOSE, cloud_close_kernel)
    return (candidate > 0).astype(np.uint8), (seed > 0).astype(np.uint8), (cloud > 0).astype(np.uint8)


def cut_narrow_channels(mask: np.ndarray, min_width: int) -> np.ndarray:
    """Cut thin bridge-like channels before connected-component growth.

    At the current 1/4 scale, a 7 px kernel cuts channels roughly below
    25-30 full-resolution pixels. The goal is to stop region growth from
    leaking through narrow gaps into reflective leaves/buildings while keeping
    broad sky areas intact.
    """
    if mask.size == 0:
        return mask
    k = max(3, int(min_width))
    if (k % 2) == 0:
        k += 1
    binary = (mask > 0).astype(np.uint8) * 255
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
    opened = cv2.morphologyEx(binary, cv2.MORPH_OPEN, kernel)
    return (opened > 0).astype(np.uint8)


def keep_seeded_components(candidate: np.ndarray, seed: np.ndarray) -> np.ndarray:
    candidate = cut_narrow_channels(candidate, min_width=5)
    count, labels, stats, _ = cv2.connectedComponentsWithStats(candidate, 8)
    out = np.zeros_like(candidate)
    h, w = candidate.shape
    min_area = max(120, int(candidate.size * 0.00025))
    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        if area < min_area:
            continue
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        component = labels[y : y + hh, x : x + ww] == lab
        seed_hits = int(np.count_nonzero(seed[y : y + hh, x : x + ww][component]))
        touches_top_band = y < int(h * 0.12) and area > min_area * 4
        if seed_hits < 8 and not touches_top_band:
            continue
        # Reflective leaf fragments tend to have complex boundaries. Reject very
        # ragged small/medium components even when they contain a few bright seed
        # pixels.
        comp_u8 = component.astype(np.uint8)
        contours, _ = cv2.findContours(comp_u8, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        perimeter = sum(cv2.arcLength(c, True) for c in contours)
        raggedness = (perimeter * perimeter) / max(1.0, float(area))
        if area < min_area * 20 and raggedness > 95.0:
            continue
        out[labels == lab] = 1
    return out


def keep_cloud_components(cloud_candidate: np.ndarray, clear_mask: np.ndarray, seed: np.ndarray) -> np.ndarray:
    cloud_candidate = cut_narrow_channels(cloud_candidate, min_width=7)
    count, labels, stats, _ = cv2.connectedComponentsWithStats(cloud_candidate, 8)
    out = np.zeros_like(cloud_candidate)
    h, w = cloud_candidate.shape
    min_area = max(300, int(cloud_candidate.size * 0.00008))

    anchor = ((clear_mask > 0) | (seed > 0)).astype(np.uint8) * 255
    anchor_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (21, 21))
    anchor = cv2.dilate(anchor, anchor_kernel, iterations=1) > 0

    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        if area < min_area:
            continue
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        component = labels[y : y + hh, x : x + ww] == lab
        anchor_hits = int(np.count_nonzero(anchor[y : y + hh, x : x + ww][component]))
        if anchor_hits < max(40, int(area * 0.06)):
            continue

        comp_u8 = component.astype(np.uint8)
        contours, _ = cv2.findContours(comp_u8, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        perimeter = sum(cv2.arcLength(c, True) for c in contours)
        raggedness = (perimeter * perimeter) / max(1.0, float(area))
        if area < min_area * 18 and raggedness > 105.0:
            continue

        out[labels == lab] = 1
    return out


def fill_small_holes(mask: np.ndarray, max_area: int) -> np.ndarray:
    """Fill only small enclosed holes inside a sky component.

    This keeps tiny object/noise holes from punching the detection mask while
    avoiding wholesale filling of tree crowns or building gaps.
    """
    if mask.size == 0:
        return mask
    inv = (mask == 0).astype(np.uint8)
    count, labels, stats, _ = cv2.connectedComponentsWithStats(inv, 8)
    out = mask.copy()
    h, w = mask.shape
    for lab in range(1, count):
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        area = int(stats[lab, cv2.CC_STAT_AREA])
        touches_border = x == 0 or y == 0 or (x + ww) >= w or (y + hh) >= h
        if touches_border or area > max_area:
            continue
        out[labels == lab] = 1
    return out


def prune_sky_fragments(mask: np.ndarray) -> np.ndarray:
    """Drop isolated tiny/irregular sky fragments from the final detection mask."""
    binary = (mask > 0).astype(np.uint8)
    count, labels, stats, _ = cv2.connectedComponentsWithStats(binary, 8)
    if count <= 1:
        return (binary * 255).astype(np.uint8)

    areas = [int(stats[lab, cv2.CC_STAT_AREA]) for lab in range(1, count)]
    largest = max(areas) if areas else 0
    total = int(np.count_nonzero(binary))
    h, w = binary.shape
    min_area_abs = max(9000, int(binary.size * 0.0012))
    min_area_rel_largest = int(largest * 0.04)
    min_area_rel_total = int(total * 0.015)
    min_area = max(min_area_abs, min_area_rel_largest, min_area_rel_total)

    out = np.zeros_like(binary)
    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        if area == largest:
            out[labels == lab] = 1
            continue
        if area < min_area:
            continue
        if ww < max(160, int(w * 0.018)) or hh < max(80, int(h * 0.035)):
            continue

        component = (labels[y : y + hh, x : x + ww] == lab).astype(np.uint8)
        contours, _ = cv2.findContours(component, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        perimeter = sum(cv2.arcLength(c, True) for c in contours)
        raggedness = (perimeter * perimeter) / max(1.0, float(area))
        fill_ratio = area / float(max(1, ww * hh))
        if area < largest * 0.16 and (raggedness > 120.0 or fill_ratio < 0.12):
            continue
        out[labels == lab] = 1

    close_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    out = cv2.morphologyEx(out * 255, cv2.MORPH_CLOSE, close_kernel)
    return out


def build_stream_mask(stream: str, paths: Sequence[Path]) -> StreamMask:
    smalls = [small_gray(path) for path in paths]
    first = smalls[0]
    th = thresholds_for(first)
    clear_frames: List[np.ndarray] = []
    cloud_frames: List[np.ndarray] = []
    sky_frames: List[np.ndarray] = []
    for small in smalls:
        candidate, seed, cloud_candidate = frame_candidate_and_seed(small, th)
        clear_mask = keep_seeded_components(candidate, seed)
        cloud_mask = keep_cloud_components(cloud_candidate, clear_mask, seed)
        sky_mask = ((clear_mask > 0) | (cloud_mask > 0)).astype(np.uint8)
        sky_mask = fill_small_holes(sky_mask, max_area=360)
        clear_frames.append(clear_mask.astype(np.uint8))
        cloud_frames.append(cloud_mask.astype(np.uint8))
        sky_frames.append(sky_mask)

    support = np.sum(np.stack(sky_frames, axis=0), axis=0).astype(np.uint8)
    clear_support = np.sum(np.stack(clear_frames, axis=0), axis=0).astype(np.uint8)
    cloud_support = np.sum(np.stack(cloud_frames, axis=0), axis=0).astype(np.uint8)
    time_std = np.std(np.stack([s.astype(np.float32) for s in smalls], axis=0), axis=0)

    first_clear = clear_frames[0].astype(np.uint8)
    first_cloud = cloud_frames[0].astype(np.uint8)
    first_sky = sky_frames[0].astype(np.uint8)
    clear_sure_small = ((first_clear > 0) & (clear_support >= 2) & (time_std <= 20.0)).astype(np.uint8)
    # Cloud support is region-level. Clouds can contain gray variation, so do
    # not require low per-pixel temporal standard deviation here.
    cloud_sure_small = ((first_cloud > 0) & (cloud_support >= 2)).astype(np.uint8)
    sure_small = ((clear_sure_small > 0) | (cloud_sure_small > 0)).astype(np.uint8)
    sure_small = fill_small_holes(sure_small, max_area=360)

    # Keep the safe mask away from reflective foliage/building boundaries.
    sure_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    sure_small = cv2.morphologyEx(sure_small * 255, cv2.MORPH_OPEN, sure_kernel)
    sure_small = (cv2.erode(sure_small, sure_kernel, iterations=1) > 0).astype(np.uint8)
    sure_small = fill_small_holes(sure_small, max_area=360).astype(np.uint8)
    sure_small = cut_narrow_channels(sure_small, min_width=7)
    clear_sure_small = ((clear_sure_small > 0) & (sure_small > 0)).astype(np.uint8) * 255
    cloud_sure_small = ((cloud_sure_small > 0) & (sure_small > 0) & (clear_sure_small == 0)).astype(np.uint8) * 255
    sure_small = prune_sky_fragments(sure_small)
    clear_sure_small = cv2.bitwise_and(clear_sure_small, sure_small)
    cloud_sure_small = cv2.bitwise_and(cloud_sure_small, sure_small)

    review_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
    review_small = cv2.dilate(first_sky * 255, review_kernel, iterations=1)
    maybe_small = cv2.subtract(review_small, sure_small)

    first_full = load_gray(paths[0])
    full_size = (first_full.shape[1], first_full.shape[0])
    sure = cv2.resize(sure_small, full_size, interpolation=cv2.INTER_NEAREST)
    clear = cv2.resize(clear_sure_small, full_size, interpolation=cv2.INTER_NEAREST)
    cloud = cv2.resize(cloud_sure_small, full_size, interpolation=cv2.INTER_NEAREST)
    maybe = cv2.resize(maybe_small, full_size, interpolation=cv2.INTER_NEAREST)
    candidate_first = cv2.resize(first_sky * 255, full_size, interpolation=cv2.INTER_NEAREST)
    support_full = cv2.resize((support * 85).astype(np.uint8), full_size, interpolation=cv2.INTER_NEAREST)

    stats = {
        "sure_pixels": float(np.count_nonzero(sure)),
        "clear_pixels": float(np.count_nonzero(clear)),
        "cloud_pixels": float(np.count_nonzero(cloud)),
        "maybe_pixels": float(np.count_nonzero(maybe)),
        "candidate_pixels": float(np.count_nonzero(candidate_first)),
        "total_pixels": float(sure.size),
        "sure_ratio": float(np.count_nonzero(sure)) / float(sure.size),
        "clear_ratio": float(np.count_nonzero(clear)) / float(sure.size),
        "cloud_ratio": float(np.count_nonzero(cloud)) / float(sure.size),
        "maybe_ratio": float(np.count_nonzero(maybe)) / float(sure.size),
        "candidate_ratio": float(np.count_nonzero(candidate_first)) / float(sure.size),
        **th,
    }
    return StreamMask(
        stream=stream,
        first_image=paths[0],
        sure=sure,
        clear=clear,
        cloud=cloud,
        maybe=maybe,
        candidate_first=candidate_first,
        temporal_support=support_full,
        temporal_std_small=time_std,
        stats=stats,
    )


def overlay_mask(gray: np.ndarray, sure: np.ndarray, maybe: np.ndarray) -> np.ndarray:
    out = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    maybe_idx = maybe > 0
    sure_idx = sure > 0
    # BGR: maybe yellow, sure cyan-blue.
    out[maybe_idx, 0] = np.clip(out[maybe_idx, 0].astype(np.float32) * 0.55 + 30 * 0.45, 0, 255).astype(np.uint8)
    out[maybe_idx, 1] = np.clip(out[maybe_idx, 1].astype(np.float32) * 0.55 + 210 * 0.45, 0, 255).astype(np.uint8)
    out[maybe_idx, 2] = np.clip(out[maybe_idx, 2].astype(np.float32) * 0.55 + 255 * 0.45, 0, 255).astype(np.uint8)
    out[sure_idx, 0] = np.clip(out[sure_idx, 0].astype(np.float32) * 0.50 + 255 * 0.50, 0, 255).astype(np.uint8)
    out[sure_idx, 1] = np.clip(out[sure_idx, 1].astype(np.float32) * 0.50 + 185 * 0.50, 0, 255).astype(np.uint8)
    out[sure_idx, 2] = np.clip(out[sure_idx, 2].astype(np.float32) * 0.50 + 30 * 0.50, 0, 255).astype(np.uint8)
    return out


def overlay_layers(gray: np.ndarray, clear: np.ndarray, cloud: np.ndarray, maybe: np.ndarray) -> np.ndarray:
    out = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    maybe_idx = maybe > 0
    cloud_idx = cloud > 0
    clear_idx = clear > 0
    # BGR colors: maybe yellow, cloud blue, clear cyan.
    out[maybe_idx, 0] = np.clip(out[maybe_idx, 0].astype(np.float32) * 0.55 + 30 * 0.45, 0, 255).astype(np.uint8)
    out[maybe_idx, 1] = np.clip(out[maybe_idx, 1].astype(np.float32) * 0.55 + 210 * 0.45, 0, 255).astype(np.uint8)
    out[maybe_idx, 2] = np.clip(out[maybe_idx, 2].astype(np.float32) * 0.55 + 255 * 0.45, 0, 255).astype(np.uint8)

    out[cloud_idx, 0] = np.clip(out[cloud_idx, 0].astype(np.float32) * 0.50 + 255 * 0.50, 0, 255).astype(np.uint8)
    out[cloud_idx, 1] = np.clip(out[cloud_idx, 1].astype(np.float32) * 0.50 + 120 * 0.50, 0, 255).astype(np.uint8)
    out[cloud_idx, 2] = np.clip(out[cloud_idx, 2].astype(np.float32) * 0.50 + 60 * 0.50, 0, 255).astype(np.uint8)

    out[clear_idx, 0] = np.clip(out[clear_idx, 0].astype(np.float32) * 0.50 + 255 * 0.50, 0, 255).astype(np.uint8)
    out[clear_idx, 1] = np.clip(out[clear_idx, 1].astype(np.float32) * 0.50 + 200 * 0.50, 0, 255).astype(np.uint8)
    out[clear_idx, 2] = np.clip(out[clear_idx, 2].astype(np.float32) * 0.50 + 30 * 0.50, 0, 255).astype(np.uint8)
    return out


def save_jpg(path: Path, image: np.ndarray, quality: int = 94) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(path), image, [int(cv2.IMWRITE_JPEG_QUALITY), int(quality)])


def make_ab_preview(paths: Sequence[Path], out_path: Path, width: int) -> None:
    panels: List[np.ndarray] = []
    for label, path in zip(("A", "B"), paths):
        img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if img is None:
            raise RuntimeError(f"failed to read preview image: {path}")
        scale = width / float(img.shape[1])
        small = cv2.resize(img, (width, max(1, int(img.shape[0] * scale))), interpolation=cv2.INTER_AREA)
        cv2.putText(small, label, (30, 90), cv2.FONT_HERSHEY_SIMPLEX, 3.0, (0, 0, 255), 8, cv2.LINE_AA)
        panels.append(small)
    save_jpg(out_path, np.vstack(panels), 95)


def write_stats(path: Path, rows: Sequence[Dict[str, object]]) -> None:
    fields = [
        "stream", "first_image", "sure_pixels", "clear_pixels", "cloud_pixels",
        "maybe_pixels", "candidate_pixels", "total_pixels", "sure_ratio",
        "clear_ratio", "cloud_ratio", "maybe_ratio", "candidate_ratio",
        "bright_candidate", "bright_seed", "grad_candidate", "grad_seed",
        "texture_candidate", "texture_seed", "lap_candidate", "cloud_bright",
        "cloud_grad", "cloud_texture", "cloud_lap", "cloud_edge_density",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main() -> int:
    args = parse_args()
    if args.clean:
        clean_output_dir(args.out_dir)
    else:
        args.out_dir.mkdir(parents=True, exist_ok=True)

    pairs = list_complete_pairs(args.source_dir, args.pairs)
    a_paths = [pair[1] for pair in pairs]
    b_paths = [pair[2] for pair in pairs]

    results = [
        build_stream_mask("A", a_paths),
        build_stream_mask("B", b_paths),
    ]

    stats_rows: List[Dict[str, object]] = []
    layer_overlay_paths: List[Path] = []
    detect_overlay_paths: List[Path] = []

    for result in results:
        gray = load_gray(result.first_image)
        overlay = overlay_layers(gray, result.clear, result.cloud, result.maybe)
        detect_overlay = overlay_mask(gray, result.sure, np.zeros_like(result.maybe))

        prefix = f"{result.stream}"
        overlay_path = args.out_dir / f"first_{prefix}_v1_clear_cloud_maybe_on_original_full.jpg"
        detect_overlay_path = args.out_dir / f"first_{prefix}_v1_detect_sky_on_original_full.jpg"
        save_jpg(overlay_path, overlay, 93)
        save_jpg(detect_overlay_path, detect_overlay, 93)
        layer_overlay_paths.append(overlay_path)
        detect_overlay_paths.append(detect_overlay_path)

        cv2.imwrite(str(args.out_dir / f"first_{prefix}_v1_detect_sky_mask.png"), result.sure)
        cv2.imwrite(str(args.out_dir / f"first_{prefix}_v1_clear_sky_mask.png"), result.clear)
        cv2.imwrite(str(args.out_dir / f"first_{prefix}_v1_cloud_sky_mask.png"), result.cloud)
        cv2.imwrite(str(args.out_dir / f"first_{prefix}_maybe_sky_mask.png"), result.maybe)
        cv2.imwrite(str(args.out_dir / f"first_{prefix}_candidate_seeded_sky_mask.png"), result.candidate_first)
        cv2.imwrite(str(args.out_dir / f"first_{prefix}_temporal_support.png"), result.temporal_support)

        row: Dict[str, object] = {"stream": result.stream, "first_image": result.first_image.name}
        row.update(result.stats)
        stats_rows.append(row)

    make_ab_preview(
        layer_overlay_paths,
        args.out_dir / "first_AB_v1_clear_cloud_maybe_on_original.jpg",
        args.ab_width,
    )
    make_ab_preview(
        detect_overlay_paths,
        args.out_dir / "first_AB_v1_detect_sky_mask_on_original.jpg",
        args.ab_width,
    )
    write_stats(args.out_dir / "sky_mask_stats.csv", stats_rows)

    with (args.out_dir / "source_pairs.txt").open("w", encoding="utf-8") as f:
        for stem, a_path, b_path in pairs:
            f.write(f"{stem}\n")
            f.write(f"  A {a_path}\n")
            f.write(f"  B {b_path}\n")

    print(f"out_dir={args.out_dir}")
    for row in stats_rows:
        print(
            f"{row['stream']}: sure={float(row['sure_ratio']):.6f} "
            f"maybe={float(row['maybe_ratio']):.6f} candidate={float(row['candidate_ratio']):.6f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
