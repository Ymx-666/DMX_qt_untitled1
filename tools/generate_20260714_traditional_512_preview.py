#!/usr/bin/env python3
"""Generate 512x512 traditional-detector preview crops for 20260714 BW data.

The script reuses the accepted v1 sky-mask implementation from
make_20260714_temporal_sky_mask.py, then runs the current traditional point
target detector inside the sky mask. It writes clean 512 crops plus a contact
sheet for quick inspection.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


DEFAULT_SOURCE_ROOT = Path("/mnt/dmx4t/data/recordings/20260714/bw")
DEFAULT_OUT_DIR = Path("/mnt/dmx4t/DMX_yangben/20260714/traditional_512_preview")
DEFAULT_REFERENCE = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis/wrj.jpg")
FALLBACK_REFERENCE = Path("/mnt/dmx4t/.Trash-1000/files/wrj.jpg")
FALLBACK_TARGET_IMAGE = Path("/mnt/dmx4t/data/recordings/20260714/bw/17/BW_20260714_170002_2366-A.jpg")
FALLBACK_TARGET_CENTER = (18455, 1558)
NAME_RE = re.compile(r"^(BW_\d{8}_(?P<hms>\d{6})_(?P<idx>\d+))-(?P<half>[AB])\.jpg$")


@dataclass
class SourceImage:
    path: Path
    hour: str
    half: str
    stem: str
    index: int


@dataclass
class Candidate:
    source: SourceImage
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


def load_sky_module():
    module_path = Path(__file__).resolve().parent / "make_20260714_temporal_sky_mask.py"
    spec = importlib.util.spec_from_file_location("dmx_temporal_sky_mask", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load sky module: {module_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse_source(path: Path) -> SourceImage | None:
    match = NAME_RE.match(path.name)
    if not match:
        return None
    return SourceImage(
        path=path,
        hour=path.parent.name,
        half=match.group("half"),
        stem=match.group(1),
        index=int(match.group("idx")),
    )


def discover_sources(source_root: Path, hours: Sequence[str]) -> List[SourceImage]:
    out: List[SourceImage] = []
    for hour in hours:
        hour_dir = source_root / hour
        if not hour_dir.is_dir():
            continue
        for path in hour_dir.glob("*.jpg"):
            item = parse_source(path)
            if item is not None:
                out.append(item)
    out.sort(key=lambda item: (item.hour, item.stem, item.half, item.index))
    return out


def context_by_group(sources: Sequence[SourceImage], mask_frames: int) -> Dict[Tuple[str, str], List[Path]]:
    grouped: Dict[Tuple[str, str], List[SourceImage]] = {}
    for item in sources:
        grouped.setdefault((item.hour, item.half), []).append(item)
    contexts: Dict[Tuple[str, str], List[Path]] = {}
    for key, items in grouped.items():
        items.sort(key=lambda item: (item.stem, item.index))
        if len(items) >= mask_frames:
            contexts[key] = [item.path for item in items[:mask_frames]]
    return contexts


def odd_kernel(value: int, minimum: int) -> int:
    value = max(minimum, int(value))
    if value % 2 == 0:
        value += 1
    return value


def fixed_crop_gray(gray: np.ndarray, cx: int, cy: int, size: int, fill: int | None = None) -> np.ndarray:
    if fill is None:
        fill = int(np.mean(gray)) if gray.size else 0
    out = np.full((size, size), fill, dtype=gray.dtype)
    half = size // 2
    x0 = max(0, cx - half)
    y0 = max(0, cy - half)
    x1 = min(gray.shape[1], cx - half + size)
    y1 = min(gray.shape[0], cy - half + size)
    if x1 <= x0 or y1 <= y0:
        return out
    dx = x0 - (cx - half)
    dy = y0 - (cy - half)
    out[dy : dy + (y1 - y0), dx : dx + (x1 - x0)] = gray[y0:y1, x0:x1]
    return out


def fixed_crop_black(gray: np.ndarray, cx: int, cy: int, size: int) -> np.ndarray:
    out = np.zeros((size, size), dtype=gray.dtype)
    half = size // 2
    x0 = max(0, cx - half)
    y0 = max(0, cy - half)
    x1 = min(gray.shape[1], cx - half + size)
    y1 = min(gray.shape[0], cy - half + size)
    if x1 <= x0 or y1 <= y0:
        return out
    dx = x0 - (cx - half)
    dy = y0 - (cy - half)
    out[dy : dy + (y1 - y0), dx : dx + (x1 - x0)] = gray[y0:y1, x0:x1]
    return out


def normalized_highpass(patch: np.ndarray) -> np.ndarray:
    if patch.size == 0:
        return np.empty((0, 0), np.float32)
    gray = patch.astype(np.uint8, copy=False)
    eq = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    hp = eq.astype(np.float32) - cv2.GaussianBlur(eq, (0, 0), 7.0).astype(np.float32)
    hp -= float(hp.mean())
    norm = float(np.linalg.norm(hp))
    if norm > 1e-6:
        hp /= norm
    return hp


def load_reference_template(path: Path, out_dir: Path, size: int = 96) -> Tuple[np.ndarray, str]:
    ref = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    used = str(path)
    if ref is None and FALLBACK_REFERENCE.is_file():
        ref = cv2.imread(str(FALLBACK_REFERENCE), cv2.IMREAD_GRAYSCALE)
        used = str(FALLBACK_REFERENCE)
    if ref is None:
        src = cv2.imread(str(FALLBACK_TARGET_IMAGE), cv2.IMREAD_GRAYSCALE)
        if src is None:
            raise RuntimeError(f"failed to read reference template: {path}")
        cx, cy = FALLBACK_TARGET_CENTER
        ref = fixed_crop_gray(src, cx, cy, 220, fill=int(np.mean(src)))
        used = f"{FALLBACK_TARGET_IMAGE}:center({cx},{cy})"
    patch = fixed_crop_gray(ref, ref.shape[1] // 2, ref.shape[0] // 2, size, fill=int(np.mean(ref)))
    preview = fixed_crop_gray(ref, ref.shape[1] // 2, ref.shape[0] // 2, 220, fill=int(np.mean(ref)))
    out_dir.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out_dir / "reference_template_used.jpg"), preview, [int(cv2.IMWRITE_JPEG_QUALITY), 95])
    return normalized_highpass(patch), used


def template_corr(gray: np.ndarray, templ: np.ndarray, cx: int, cy: int, fill: int) -> float:
    if templ.size == 0:
        return 0.0
    patch = fixed_crop_gray(gray, cx, cy, templ.shape[1], fill=fill)
    hp = normalized_highpass(patch)
    if hp.shape != templ.shape:
        return 0.0
    return float(np.sum(hp * templ))


def point_features(gray: np.ndarray, response: np.ndarray, cx: int, cy: int, fill: int, size: int = 96) -> Tuple[float, float]:
    patch = fixed_crop_gray(gray, cx, cy, size, fill=fill)
    resp = fixed_crop_gray(response, cx, cy, size, fill=0).astype(np.float32)
    yy, xx = np.mgrid[0:size, 0:size]
    dx = xx - size / 2.0
    dy = yy - size / 2.0
    inner = ((dx / 13.0) ** 2 + (dy / 8.0) ** 2) <= 1.0
    outer = ((dx / 36.0) ** 2 + (dy / 24.0) ** 2) <= 1.0
    ring = outer & ~inner
    inner_mean = float(np.mean(patch[inner])) if np.any(inner) else 0.0
    ring_mean = float(np.mean(patch[ring])) if np.any(ring) else inner_mean
    resp_mean = float(np.mean(resp[inner])) if np.any(inner) else 0.0
    contrast = ring_mean - inner_mean
    center_ring = resp_mean * 2.0 + contrast
    return contrast, center_ring


def far_enough(kept: Sequence[Candidate], cand: Candidate, radius: int) -> bool:
    r2 = radius * radius
    for item in kept:
        dx = item.x - cand.x
        dy = item.y - cand.y
        if dx * dx + dy * dy <= r2:
            return False
    return True


def globally_far_enough(centers: Sequence[Tuple[int, int]], x: int, y: int, radius: int) -> bool:
    if radius <= 0:
        return True
    r2 = radius * radius
    for px, py in centers:
        dx = px - x
        dy = py - y
        if dx * dx + dy * dy <= r2:
            return False
    return True


def detect_candidates(gray_in: np.ndarray,
                      sky_mask: np.ndarray,
                      source: SourceImage,
                      templ: np.ndarray,
                      max_candidates: int) -> List[Candidate]:
    gray = gray_in
    if gray.ndim != 2:
        gray = cv2.cvtColor(gray, cv2.COLOR_BGR2GRAY)
    gray = cv2.medianBlur(gray, 3)
    gray_fill = int(np.mean(gray))

    detection_mask = sky_mask.copy()
    edge_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    eroded = cv2.erode(sky_mask, edge_kernel, iterations=1)
    if cv2.countNonZero(eroded) > max(1024, cv2.countNonZero(sky_mask) // 5):
        detection_mask = eroded
    if cv2.countNonZero(detection_mask) <= 0:
        return []

    local_bg = cv2.GaussianBlur(gray, (0, 0), 9.0)
    dark_response = cv2.subtract(local_bg, gray)
    eq = cv2.createCLAHE(clipLimit=1.8, tileGridSize=(8, 8)).apply(gray)
    eq_blur = cv2.GaussianBlur(eq, (0, 0), 1.2)
    sharp = cv2.addWeighted(eq, 1.45, eq_blur, -0.45, 0.0)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (odd_kernel(15, 9), odd_kernel(15, 9)))
    blackhat = cv2.morphologyEx(sharp, cv2.MORPH_BLACKHAT, kernel)
    response = np.maximum(dark_response, blackhat)
    response = cv2.bitwise_and(response, detection_mask)

    mean, std = cv2.meanStdDev(response, mask=detection_mask)
    threshold = float(mean[0][0]) + 3.5 * float(std[0][0])
    threshold = max(threshold, max(8.0, 25.0 * 0.40))
    threshold = min(threshold, 90.0)
    mask = (response >= threshold).astype(np.uint8) * 255
    mask = cv2.bitwise_and(mask, detection_mask)
    close_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, close_kernel)

    count, labels, stats, centroids = cv2.connectedComponentsWithStats(mask, 8)
    pool: List[Candidate] = []
    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        if area < 8 or area > 400:
            continue
        x = int(stats[lab, cv2.CC_STAT_LEFT])
        y = int(stats[lab, cv2.CC_STAT_TOP])
        ww = int(stats[lab, cv2.CC_STAT_WIDTH])
        hh = int(stats[lab, cv2.CC_STAT_HEIGHT])
        if ww <= 0 or hh <= 0 or ww > 96 or hh > 96:
            continue
        aspect = ww / float(max(1, hh))
        if aspect < 0.50 or aspect > 2.60:
            continue
        compactness = area / float(max(1, ww * hh))
        if compactness < 0.35:
            continue
        cx = int(round(float(centroids[lab][0])))
        cy = int(round(float(centroids[lab][1])))
        if cx < 0 or cy < 0 or cx >= gray.shape[1] or cy >= gray.shape[0]:
            continue
        if detection_mask[cy, cx] == 0:
            continue
        inner = (labels[y : y + hh, x : x + ww] == lab)
        mask_roi = detection_mask[y : y + hh, x : x + ww]
        if float(np.mean(mask_roi[inner])) < 220.0:
            continue
        response_max = float(np.max(response[y : y + hh, x : x + ww][inner]))
        contrast, center_ring = point_features(gray, response, cx, cy, gray_fill)
        if contrast < 6.0 or center_ring < 22.0:
            continue
        corr = template_corr(gray, templ, cx, cy, gray_fill)
        if corr < 0.18:
            continue
        line_penalty = max(aspect, 1.0 / max(0.001, aspect))
        if line_penalty > 2.60:
            continue
        score = response_max + contrast * 2.0 + center_ring * 2.0 + max(0.0, corr) * 140.0 + compactness * 20.0
        if line_penalty > 2.20:
            score *= 0.85
        if score < 115.0:
            continue
        pool.append(Candidate(
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
        if not far_enough(kept, cand, 96):
            continue
        kept.append(cand)
        if len(kept) >= max_candidates:
            break
    return kept


def clean_output(out_dir: Path, save_debug_overlays: bool) -> None:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    (out_dir / "images").mkdir(parents=True, exist_ok=True)
    if save_debug_overlays:
        (out_dir / "debug").mkdir(parents=True, exist_ok=True)


def write_manifest_header(path: Path) -> None:
    fields = [
        "file", "source", "hour", "half", "source_index", "center_x", "center_y",
        "bbox_x1", "bbox_y1", "bbox_x2", "bbox_y2", "crop_size", "score",
        "response", "contrast", "center_ring", "template_corr", "area", "aspect",
        "compactness",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerow(fields)


def append_manifest(path: Path, rows: Iterable[Sequence[object]]) -> None:
    with path.open("a", newline="", encoding="utf-8") as f:
        csv.writer(f).writerows(rows)


def save_crop(out_dir: Path, idx: int, cand: Candidate, gray: np.ndarray, crop_size: int) -> Tuple[Path, np.ndarray]:
    crop_gray = fixed_crop_black(gray, cand.x, cand.y, crop_size)
    crop_bgr = cv2.cvtColor(crop_gray, cv2.COLOR_GRAY2BGR)
    name = (
        f"{idx:03d}_{cand.source.hour}_{cand.source.stem}-{cand.source.half}_"
        f"cx{cand.x:05d}_cy{cand.y:04d}_score{cand.score:.1f}.jpg"
    )
    out_path = out_dir / "images" / name
    cv2.imwrite(str(out_path), crop_bgr, [int(cv2.IMWRITE_JPEG_QUALITY), 95])
    return out_path, crop_bgr


def save_debug_overlay(out_dir: Path, source: SourceImage, gray: np.ndarray, candidates: Sequence[Candidate]) -> None:
    if not candidates:
        return
    scale_width = 2400
    scale = min(1.0, scale_width / float(gray.shape[1]))
    vis = cv2.cvtColor(cv2.resize(gray, (int(gray.shape[1] * scale), int(gray.shape[0] * scale)), interpolation=cv2.INTER_AREA), cv2.COLOR_GRAY2BGR)
    for cand in candidates:
        x1 = int((cand.x1 - 14) * scale)
        y1 = int((cand.y1 - 14) * scale)
        x2 = int((cand.x2 + 14) * scale)
        y2 = int((cand.y2 + 14) * scale)
        cv2.rectangle(vis, (x1, y1), (x2, y2), (0, 0, 255), 2)
        cv2.putText(vis, f"{cand.score:.1f}", (max(0, x1), max(24, y1 - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 255), 1, cv2.LINE_AA)
    cv2.imwrite(str(out_dir / "debug" / f"{source.stem}-{source.half}_boxes.jpg"), vis, [int(cv2.IMWRITE_JPEG_QUALITY), 90])


def make_contact_sheet(crops: Sequence[Tuple[Path, np.ndarray]], out_path: Path) -> None:
    if not crops:
        return
    cell = 512
    cols = 5
    rows = int(np.ceil(len(crops) / float(cols)))
    sheet = np.zeros((rows * cell, cols * cell, 3), dtype=np.uint8)
    for idx, (path, crop) in enumerate(crops):
        r = idx // cols
        c = idx % cols
        vis = crop.copy()
        center = cell // 2
        cv2.drawMarker(vis, (center, center), (0, 0, 255), markerType=cv2.MARKER_CROSS, markerSize=26, thickness=2)
        cv2.putText(vis, path.name[:48], (10, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.48, (0, 0, 255), 1, cv2.LINE_AA)
        sheet[r * cell : (r + 1) * cell, c * cell : (c + 1) * cell] = vis
    cv2.imwrite(str(out_path), sheet, [int(cv2.IMWRITE_JPEG_QUALITY), 94])


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--hours", nargs="+", default=["16", "17", "18"])
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    parser.add_argument("--mask-frames", type=int, default=3)
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument("--max-crops", type=int, default=10, help="Maximum saved crops; 0 means process all accepted candidates.")
    parser.add_argument("--per-image-max", type=int, default=3)
    parser.add_argument("--global-dedup-radius", type=int, default=384)
    parser.add_argument("--save-debug-overlays", action="store_true", help="Save per-source full-image debug overlays. Off by default for full dataset runs.")
    parser.add_argument("--contact-sheet-limit", type=int, default=30, help="Number of saved crops included in preview_contact_sheet.jpg; 0 disables it.")
    parser.add_argument("--clean", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--progress-every", type=int, default=5)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.clean:
        clean_output(args.out_dir, args.save_debug_overlays)
    else:
        (args.out_dir / "images").mkdir(parents=True, exist_ok=True)
        if args.save_debug_overlays:
            (args.out_dir / "debug").mkdir(parents=True, exist_ok=True)

    sky = load_sky_module()
    sources = discover_sources(args.source_root, args.hours)
    contexts = context_by_group(sources, args.mask_frames)
    if not sources:
        raise RuntimeError(f"no source images found under {args.source_root} hours={args.hours}")
    templ, reference_used = load_reference_template(args.reference, args.out_dir)
    print(f"[reference] {reference_used}", flush=True)

    manifest = args.out_dir / "manifest.csv"
    write_manifest_header(manifest)
    mask_cache: Dict[Tuple[str, str], np.ndarray] = {}
    contact_crops: List[Tuple[Path, np.ndarray]] = []
    rows: List[List[object]] = []
    processed = 0
    saved_count = 0
    accepted_centers: Dict[Tuple[str, str], List[Tuple[int, int]]] = {}
    processed_by_group: Dict[str, int] = {}
    saved_by_group: Dict[str, int] = {}

    for source in sources:
        key = (source.hour, source.half)
        context = contexts.get(key)
        if not context:
            continue
        if key not in mask_cache:
            print(f"[mask] hour={source.hour} half={source.half} frames={[p.name for p in context]}", flush=True)
            mask_cache[key] = sky.build_stream_mask(source.half, context).sure

        gray = cv2.imread(str(source.path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            print(f"[WARN] read failed: {source.path}", flush=True)
            continue
        candidates = detect_candidates(gray, mask_cache[key], source, templ, args.per_image_max)
        processed += 1
        group_name = f"{source.hour}{source.half}"
        processed_by_group[group_name] = processed_by_group.get(group_name, 0) + 1
        if candidates and args.save_debug_overlays:
            save_debug_overlay(args.out_dir, source, gray, candidates)
        saved_this_source = 0
        for cand in candidates:
            group_centers = accepted_centers.setdefault(key, [])
            if not globally_far_enough(group_centers, cand.x, cand.y, args.global_dedup_radius):
                continue
            crop_path, crop = save_crop(args.out_dir, saved_count + 1, cand, gray, args.crop_size)
            group_centers.append((cand.x, cand.y))
            saved_this_source += 1
            saved_count += 1
            saved_by_group[group_name] = saved_by_group.get(group_name, 0) + 1
            if args.contact_sheet_limit > 0 and len(contact_crops) < args.contact_sheet_limit:
                contact_crops.append((crop_path, crop))
            rows.append([
                str(crop_path.relative_to(args.out_dir)),
                str(cand.source.path),
                cand.source.hour,
                cand.source.half,
                cand.source.index,
                cand.x,
                cand.y,
                cand.x1,
                cand.y1,
                cand.x2,
                cand.y2,
                args.crop_size,
                f"{cand.score:.4f}",
                f"{cand.response:.4f}",
                f"{cand.contrast:.4f}",
                f"{cand.center_ring:.4f}",
                f"{cand.template_corr:.6f}",
                cand.area,
                f"{cand.aspect:.4f}",
                f"{cand.compactness:.4f}",
            ])
            append_manifest(manifest, [rows[-1]])
            total_label = "all" if args.max_crops <= 0 else str(args.max_crops)
            print(f"[crop {saved_count:04d}/{total_label}] {crop_path.name}", flush=True)
            if args.max_crops > 0 and saved_count >= args.max_crops:
                break
            if saved_this_source >= 1:
                break
        if args.max_crops > 0 and saved_count >= args.max_crops:
            break
        if processed % args.progress_every == 0:
            print(f"[progress] processed={processed} crops={saved_count} last={source.path.name}", flush=True)

    make_contact_sheet(contact_crops, args.out_dir / "preview_contact_sheet.jpg")
    summary = {
        "source_root": str(args.source_root),
        "hours": args.hours,
        "out_dir": str(args.out_dir),
        "reference": reference_used,
        "processed_images": processed,
        "saved_crops": saved_count,
        "crop_size": args.crop_size,
        "max_crops": args.max_crops,
        "per_image_max": args.per_image_max,
        "global_dedup_radius": args.global_dedup_radius,
        "save_debug_overlays": args.save_debug_overlays,
        "contact_sheet_limit": args.contact_sheet_limit,
        "processed_by_group": processed_by_group,
        "saved_by_group": saved_by_group,
        "manifest": str(manifest),
    }
    (args.out_dir / "run_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"processed_images={processed}")
    print(f"saved_crops={saved_count}")
    print(f"out_dir={args.out_dir}")
    print(f"manifest={manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
