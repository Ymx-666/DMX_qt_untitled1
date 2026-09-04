#!/usr/bin/env python3
"""Post-filter 20260714 traditional point candidates without YOLO.

This tool is intentionally offline. It consumes the traditional candidate CSVs
already generated in the analysis directory, applies a strict point-target
filter, and uses nearby frames only to confirm medium-confidence candidates.
Strong single-frame candidates are kept so a real one-frame target is not
discarded by an overly strict temporal rule.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


DEFAULT_SOURCE = Path("/mnt/dmx4t/data/recordings/20260714/bw/17")
DEFAULT_ANALYSIS = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis")
DEFAULT_REFERENCE = DEFAULT_ANALYSIS / "wrj.jpg"
DEFAULT_STEM = "BW_20260714_170002_2366"


@dataclass
class Candidate:
    idx: int
    stream: str
    score: float
    x: int
    y: int
    x1: int
    y1: int
    x2: int
    y2: int
    area: float
    w: float
    h: float
    aspect: float
    contrast: float
    response: float
    center_ring: float
    template_corr: float
    line_penalty: float
    compactness: float
    decision: str = "reject"
    reason: str = ""
    temporal_support: int = 0
    temporal_best_corr: float = 0.0
    temporal_best_contrast: float = 0.0
    temporal_best_frame: str = ""


@dataclass
class FrameHit:
    file: str
    stream: str
    frame_index: int
    cand_idx: int
    corr: float
    contrast: float
    center_ring: float
    x: int
    y: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--analysis-dir", type=Path, default=DEFAULT_ANALYSIS)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    parser.add_argument("--stem", default=DEFAULT_STEM)
    parser.add_argument("--max-pairs", type=int, default=7)
    parser.add_argument("--temporal-radius", type=int, default=96)
    parser.add_argument("--temporal-min-support", type=int, default=2)
    return parser.parse_args()


def read_candidates(path: Path) -> List[Candidate]:
    out: List[Candidate] = []
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            out.append(
                Candidate(
                    idx=int(float(row["idx"])),
                    stream=row["stream"],
                    score=float(row["score"]),
                    x=int(float(row["x"])),
                    y=int(float(row["y"])),
                    x1=int(float(row["x1"])),
                    y1=int(float(row["y1"])),
                    x2=int(float(row["x2"])),
                    y2=int(float(row["y2"])),
                    area=float(row["area"]),
                    w=float(row["w"]),
                    h=float(row["h"]),
                    aspect=float(row["aspect"]),
                    contrast=float(row["contrast"]),
                    response=float(row["response"]),
                    center_ring=float(row["center_ring"]),
                    template_corr=float(row["template_corr"]),
                    line_penalty=float(row["line_penalty"]),
                    compactness=float(row["compactness"]),
                )
            )
    return out


def is_strong(c: Candidate) -> bool:
    return (
        c.score >= 150.0
        and c.template_corr >= 0.25
        and c.center_ring >= 35.0
        and c.contrast >= 10.0
        and 0.7 <= c.aspect <= 2.2
        and 60.0 <= c.area <= 180.0
        and c.compactness >= 0.55
    )


def is_medium(c: Candidate) -> bool:
    return (
        c.score >= 80.0
        and c.template_corr >= 0.15
        and c.center_ring >= 18.0
        and c.contrast >= 5.0
        and 0.6 <= c.aspect <= 2.2
        and 40.0 <= c.area <= 240.0
        and c.compactness >= 0.45
    )


def reject_reason(c: Candidate) -> str:
    reasons: List[str] = []
    if c.template_corr < 0.15:
        reasons.append("low_template")
    if c.center_ring < 18.0:
        reasons.append("weak_center_ring")
    if c.contrast < 5.0:
        reasons.append("low_contrast")
    if not (0.6 <= c.aspect <= 2.2):
        reasons.append("line_or_flat_shape")
    if not (40.0 <= c.area <= 240.0):
        reasons.append("bad_area")
    if c.compactness < 0.45:
        reasons.append("not_compact")
    return "+".join(reasons) if reasons else "below_strong_threshold"


def list_stream_frames(source_dir: Path, stream: str, max_pairs: int) -> List[Path]:
    pattern = re.compile(r"^BW_\d{8}_\d{6}_\d+-%s\.jpg$" % re.escape(stream))
    frames = [p for p in source_dir.glob("*.jpg") if pattern.match(p.name)]
    frames.sort()
    return frames[: max(1, max_pairs)]


def center_crop(gray: np.ndarray, cx: int, cy: int, size: int, fill: int | None = None) -> np.ndarray:
    if fill is None:
        fill = int(np.median(gray))
    half = size // 2
    x0 = max(0, cx - half)
    y0 = max(0, cy - half)
    x1 = min(gray.shape[1], cx + half)
    y1 = min(gray.shape[0], cy + half)
    crop = gray[y0:y1, x0:x1]
    out = np.full((size, size), fill, dtype=gray.dtype)
    ox = (size - crop.shape[1]) // 2
    oy = (size - crop.shape[0]) // 2
    out[oy : oy + crop.shape[0], ox : ox + crop.shape[1]] = crop
    return out


def make_ref_patch(reference_path: Path, size: int = 96) -> np.ndarray:
    ref = cv2.imread(str(reference_path), cv2.IMREAD_GRAYSCALE)
    if ref is None:
        raise RuntimeError(f"failed to read reference: {reference_path}")
    patch = center_crop(ref, ref.shape[1] // 2, ref.shape[0] // 2, size)
    patch = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(patch)
    hp = patch.astype(np.float32) - cv2.GaussianBlur(patch, (0, 0), 7).astype(np.float32)
    hp -= float(hp.mean())
    norm = float(np.linalg.norm(hp))
    if norm > 1e-6:
        hp /= norm
    return hp


def point_features(gray: np.ndarray, cx: int, cy: int, size: int = 96) -> Tuple[float, float]:
    patch = center_crop(gray, cx, cy, size)
    yy, xx = np.mgrid[0:size, 0:size]
    dx = xx - size / 2.0
    dy = yy - size / 2.0
    inner = ((dx / 13.0) ** 2 + (dy / 8.0) ** 2) <= 1.0
    outer = ((dx / 36.0) ** 2 + (dy / 24.0) ** 2) <= 1.0
    ring = outer & ~inner
    contrast = float(np.median(patch[ring]) - np.mean(patch[inner]))

    bg = cv2.GaussianBlur(gray, (0, 0), 9)
    resp = np.maximum(bg.astype(np.int16) - gray.astype(np.int16), 0).astype(np.float32)
    resp_patch = center_crop(resp.astype(np.uint8), cx, cy, size, fill=0).astype(np.float32)
    center_ring = float(np.mean(resp_patch[inner]) * 2.0 + contrast)
    return contrast, center_ring


def local_template_search(gray: np.ndarray,
                          ref_hp: np.ndarray,
                          cx: int,
                          cy: int,
                          radius: int) -> Tuple[float, int, int, float, float]:
    size = ref_hp.shape[0]
    half = size // 2
    x0 = max(0, cx - radius - half)
    y0 = max(0, cy - radius - half)
    x1 = min(gray.shape[1], cx + radius + half)
    y1 = min(gray.shape[0], cy + radius + half)
    roi = gray[y0:y1, x0:x1]
    if roi.shape[0] < size or roi.shape[1] < size:
        return 0.0, cx, cy, 0.0, 0.0

    roi_eq = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(roi)
    roi_hp = roi_eq.astype(np.float32) - cv2.GaussianBlur(roi_eq, (0, 0), 7).astype(np.float32)
    tmpl = ref_hp.astype(np.float32)
    res = cv2.matchTemplate(roi_hp, tmpl, cv2.TM_CCOEFF_NORMED)
    _, maxv, _, maxloc = cv2.minMaxLoc(res)
    hit_x = int(x0 + maxloc[0] + half)
    hit_y = int(y0 + maxloc[1] + half)
    contrast, center_ring = point_features(gray, hit_x, hit_y, size)
    return float(maxv), hit_x, hit_y, contrast, center_ring


def temporal_support(c: Candidate,
                     frames: Sequence[Path],
                     ref_hp: np.ndarray,
                     source_frame_name: str,
                     radius: int) -> Tuple[int, float, float, str, List[FrameHit]]:
    hits: List[FrameHit] = []
    best_corr = 0.0
    best_contrast = 0.0
    best_frame = ""

    for frame_index, path in enumerate(frames):
        if path.name == source_frame_name:
            continue
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            continue
        corr, hx, hy, contrast, center_ring = local_template_search(gray, ref_hp, c.x, c.y, radius)
        if corr > best_corr:
            best_corr = corr
            best_contrast = contrast
            best_frame = path.name
        supported = corr >= 0.30 and contrast >= 3.0 and center_ring >= 6.0
        if supported:
            hits.append(
                FrameHit(
                    file=path.name,
                    stream=c.stream,
                    frame_index=frame_index,
                    cand_idx=c.idx,
                    corr=corr,
                    contrast=contrast,
                    center_ring=center_ring,
                    x=hx,
                    y=hy,
                )
            )
    return len(hits), best_corr, best_contrast, best_frame, hits


def classify_candidates(candidates: List[Candidate],
                        frames_by_stream: Dict[str, List[Path]],
                        ref_hp: np.ndarray,
                        stem: str,
                        radius: int,
                        min_support: int) -> List[FrameHit]:
    all_hits: List[FrameHit] = []
    for c in candidates:
        if is_strong(c):
            c.decision = "accept"
            c.reason = "strong_single_frame"
            continue

        if is_medium(c):
            source_frame_name = f"{stem}-{c.stream}.jpg"
            support, best_corr, best_contrast, best_frame, hits = temporal_support(
                c, frames_by_stream.get(c.stream, []), ref_hp, source_frame_name, radius
            )
            c.temporal_support = support
            c.temporal_best_corr = best_corr
            c.temporal_best_contrast = best_contrast
            c.temporal_best_frame = best_frame
            all_hits.extend(hits)
            if support >= min_support:
                c.decision = "accept"
                c.reason = "medium_temporal_confirmed"
            else:
                c.decision = "reject"
                c.reason = "medium_without_temporal_support"
            continue

        c.decision = "reject"
        c.reason = reject_reason(c)
    return all_hits


def draw_results(image_path: Path, candidates: Sequence[Candidate], out_path: Path, title: str) -> None:
    gray = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
    if gray is None:
        raise RuntimeError(f"failed to read image: {image_path}")
    vis = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    accepted = [c for c in candidates if c.decision == "accept"]
    for c in accepted:
        pad = 10
        x1 = max(0, c.x1 - pad)
        y1 = max(0, c.y1 - pad)
        x2 = min(vis.shape[1] - 1, c.x2 + pad)
        y2 = min(vis.shape[0] - 1, c.y2 + pad)
        cv2.rectangle(vis, (x1, y1), (x2, y2), (0, 0, 255), 3)
        label = f"#{c.idx} {c.reason} S{c.score:.1f}"
        cv2.putText(vis, label, (max(0, x1 - 12), max(32, y1 - 18)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.85, (0, 0, 255), 2, cv2.LINE_AA)
    cv2.putText(vis, f"{title}: kept {len(accepted)} / {len(candidates)}",
                (30, 70), cv2.FONT_HERSHEY_SIMPLEX, 1.3, (0, 0, 255), 3, cv2.LINE_AA)
    cv2.imwrite(str(out_path), vis)


def write_decisions(path: Path, candidates: Sequence[Candidate]) -> None:
    fields = [
        "idx", "stream", "decision", "reason", "score", "x", "y", "x1", "y1", "x2", "y2",
        "area", "w", "h", "aspect", "contrast", "response", "center_ring",
        "template_corr", "line_penalty", "compactness", "temporal_support",
        "temporal_best_corr", "temporal_best_contrast", "temporal_best_frame",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for c in candidates:
            writer.writerow({field: getattr(c, field) for field in fields})


def write_temporal_hits(path: Path, hits: Sequence[FrameHit]) -> None:
    fields = ["file", "stream", "frame_index", "cand_idx", "corr", "contrast", "center_ring", "x", "y"]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for hit in hits:
            writer.writerow({field: getattr(hit, field) for field in fields})


def make_overview(paths: Sequence[Path], out_path: Path, panel_width: int = 2400) -> None:
    panels: List[np.ndarray] = []
    for path in paths:
        img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if img is None:
            continue
        scale = panel_width / float(img.shape[1])
        panels.append(cv2.resize(img, (panel_width, max(1, int(img.shape[0] * scale))), interpolation=cv2.INTER_AREA))
    if panels:
        cv2.imwrite(str(out_path), np.vstack(panels))


def make_accepted_crop_sheet(source_dir: Path,
                             stem: str,
                             candidates: Sequence[Candidate],
                             out_path: Path,
                             crop_size: int = 220,
                             scale: int = 3) -> None:
    accepted = [c for c in candidates if c.decision == "accept"]
    if not accepted:
        return
    cells: List[np.ndarray] = []
    for c in accepted:
        img = cv2.imread(str(source_dir / f"{stem}-{c.stream}.jpg"), cv2.IMREAD_GRAYSCALE)
        if img is None:
            continue
        crop = center_crop(img, c.x, c.y, crop_size)
        crop = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(crop)
        vis = cv2.cvtColor(crop, cv2.COLOR_GRAY2BGR)
        center = crop_size // 2
        box_w = max(8, int(c.x2 - c.x1))
        box_h = max(8, int(c.y2 - c.y1))
        cv2.rectangle(vis, (center - box_w // 2, center - box_h // 2),
                      (center + box_w // 2, center + box_h // 2), (0, 0, 255), 1)
        vis = cv2.resize(vis, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST)
        cv2.putText(vis, f"{c.stream} #{c.idx} {c.reason}", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 255), 2, cv2.LINE_AA)
        cells.append(vis)
    if cells:
        cv2.imwrite(str(out_path), np.hstack(cells))


def main() -> int:
    args = parse_args()
    args.analysis_dir.mkdir(parents=True, exist_ok=True)

    ref_hp = make_ref_patch(args.reference)
    frames_by_stream = {
        "A": list_stream_frames(args.source_dir, "A", args.max_pairs),
        "B": list_stream_frames(args.source_dir, "B", args.max_pairs),
    }

    all_candidates: List[Candidate] = []
    for stream in ("A", "B"):
        csv_path = args.analysis_dir / f"{args.stem}-{stream}_traditional_point_candidates.csv"
        if not csv_path.is_file():
            raise RuntimeError(f"missing candidate CSV: {csv_path}")
        all_candidates.extend(read_candidates(csv_path))

    temporal_hits = classify_candidates(
        all_candidates,
        frames_by_stream,
        ref_hp,
        args.stem,
        args.temporal_radius,
        args.temporal_min_support,
    )

    write_decisions(args.analysis_dir / "traditional_fp_reduction_decisions.csv", all_candidates)
    write_temporal_hits(args.analysis_dir / "traditional_fp_reduction_temporal_hits.csv", temporal_hits)

    result_paths: List[Path] = []
    for stream in ("A", "B"):
        stream_candidates = [c for c in all_candidates if c.stream == stream]
        image_path = args.source_dir / f"{args.stem}-{stream}.jpg"
        out_path = args.analysis_dir / f"{args.stem}-{stream}_traditional_fp_reduction_final_on_original.jpg"
        draw_results(image_path, stream_candidates, out_path, f"{stream} traditional fp reduction")
        result_paths.append(out_path)

    make_overview(result_paths, args.analysis_dir / "00_AB_traditional_fp_reduction_final_overview.jpg")
    make_accepted_crop_sheet(
        args.source_dir,
        args.stem,
        all_candidates,
        args.analysis_dir / "traditional_fp_reduction_accepted_crops.jpg",
    )

    accepted = [c for c in all_candidates if c.decision == "accept"]
    print(f"frames A={len(frames_by_stream['A'])} B={len(frames_by_stream['B'])}")
    print(f"candidates total={len(all_candidates)} accepted={len(accepted)}")
    for stream in ("A", "B"):
        total = sum(1 for c in all_candidates if c.stream == stream)
        kept = [c for c in all_candidates if c.stream == stream and c.decision == "accept"]
        print(f"{stream}: kept {len(kept)} / {total}; idx={[c.idx for c in kept]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
