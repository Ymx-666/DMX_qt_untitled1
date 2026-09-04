#!/usr/bin/env python3
"""Evaluate traditional small-target mining on the 20161018-4 crop dataset."""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple

import cv2
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402


DEFAULT_SOURCE = Path("/mnt/dmx4t/DMX_yangben/20161018-4")
DEFAULT_FIG_DIR = Path("/home/sht/work/DMX_qt/latex/figures/progress_report")
DEFAULT_DATA_DIR = Path("/home/sht/work/DMX_qt/latex/data/progress_report")


@dataclass
class Candidate:
    x: int
    y: int
    w: int
    h: int
    area: int
    cx: float
    cy: float
    score: float
    contrast: float
    max_response: float


@dataclass
class ImageResult:
    path: Path
    prefix: str
    day: str
    time_text: str
    pano_x: int
    pano_y: int
    candidates: List[Candidate]
    response_mean: float
    response_std: float
    response_max: float
    threshold: float


def parse_name(path: Path) -> Tuple[str, str, str, int, int]:
    match = re.match(r"^(I\d+)_(\d{8})_(\d{6})_(\d+)_(\d+)$", path.stem)
    if not match:
        return ("UNK", "", "", -1, -1)
    return (
        match.group(1),
        match.group(2),
        match.group(3),
        int(match.group(4)),
        int(match.group(5)),
    )


def list_images(src: Path) -> List[Path]:
    return sorted(p for p in src.glob("*.bmp") if p.is_file())


def nms(candidates: Sequence[Candidate], radius: float) -> List[Candidate]:
    kept: List[Candidate] = []
    for c in sorted(candidates, key=lambda item: item.score, reverse=True):
        if all(math.hypot(c.cx - k.cx, c.cy - k.cy) > radius for k in kept):
            kept.append(c)
    return kept


def detect(gray: np.ndarray,
           median_kernel: int = 3,
           tophat_kernel: int = 15,
           threshold_k: float = 3.5,
           min_area: int = 2,
           max_area: int = 180,
           min_contrast: float = 10.0,
           nms_radius: float = 18.0) -> Tuple[List[Candidate], dict]:
    if gray.ndim != 2:
        gray = cv2.cvtColor(gray, cv2.COLOR_BGR2GRAY)

    smooth = cv2.medianBlur(gray, median_kernel) if median_kernel > 1 else gray.copy()
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (tophat_kernel, tophat_kernel))
    opened = cv2.morphologyEx(smooth, cv2.MORPH_OPEN, kernel)
    closed = cv2.morphologyEx(smooth, cv2.MORPH_CLOSE, kernel)
    bright_response = cv2.subtract(smooth, opened)
    dark_response = cv2.subtract(closed, smooth)
    response = cv2.max(bright_response, dark_response)
    # The historical 256x256 crops often contain a bright bottom border from
    # acquisition/visualization. It is not a flying-object cue.
    response[max(0, response.shape[0] - 8):, :] = 0

    mean = float(response.mean())
    std = float(response.std())
    threshold = mean + threshold_k * std
    _, binary = cv2.threshold(response, threshold, 255, cv2.THRESH_BINARY)

    labels_n, labels, stats, centroids = cv2.connectedComponentsWithStats(binary.astype(np.uint8), 8)
    candidates: List[Candidate] = []
    for lab in range(1, labels_n):
        x, y, w, h, area = [int(v) for v in stats[lab, :5]]
        if area < min_area or area > max_area:
            continue
        if y + h >= gray.shape[0] - 8:
            continue
        cx, cy = [float(v) for v in centroids[lab]]
        x0 = max(0, x - 8)
        y0 = max(0, y - 8)
        x1 = min(gray.shape[1], x + w + 8)
        y1 = min(gray.shape[0], y + h + 8)
        local = gray[y0:y1, x0:x1]
        obj_mask = labels[y:y + h, x:x + w] == lab
        obj_values = gray[y:y + h, x:x + w][obj_mask]
        if obj_values.size == 0 or local.size == 0:
            continue
        obj_mean = float(obj_values.mean())
        bg_mean = float(local.mean())
        contrast = abs(obj_mean - bg_mean)
        max_response = float(response[y:y + h, x:x + w][obj_mask].max())
        score = max_response + 1.5 * contrast + 0.25 * area
        if contrast < min_contrast:
            continue
        candidates.append(Candidate(x, y, w, h, area, cx, cy, score, contrast, max_response))

    meta = {
        "smooth": smooth,
        "response": response,
        "binary": binary,
        "mean": mean,
        "std": std,
        "threshold": threshold,
        "response_max": float(response.max()),
    }
    return nms(candidates, nms_radius), meta


def evaluate(paths: Sequence[Path]) -> Tuple[List[ImageResult], dict]:
    results: List[ImageResult] = []
    prefix_counts = {}
    detected_counts = {}
    for path in paths:
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            continue
        prefix, day, time_text, pano_x, pano_y = parse_name(path)
        cands, meta = detect(gray)
        results.append(
            ImageResult(
                path=path,
                prefix=prefix,
                day=day,
                time_text=time_text,
                pano_x=pano_x,
                pano_y=pano_y,
                candidates=cands,
                response_mean=float(meta["mean"]),
                response_std=float(meta["std"]),
                response_max=float(meta["response_max"]),
                threshold=float(meta["threshold"]),
            )
        )
        prefix_counts[prefix] = prefix_counts.get(prefix, 0) + 1
        if cands:
            detected_counts[prefix] = detected_counts.get(prefix, 0) + 1
    summary = {
        "images": len(results),
        "detected_images": sum(1 for r in results if r.candidates),
        "total_candidates": sum(len(r.candidates) for r in results),
        "prefix_counts": prefix_counts,
        "detected_counts": detected_counts,
    }
    return results, summary


def draw_boxed(gray: np.ndarray, candidates: Sequence[Candidate]) -> np.ndarray:
    out = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    for c in candidates:
        cv2.rectangle(out, (c.x, c.y), (c.x + c.w, c.y + c.h), (0, 0, 255), 1)
        cv2.circle(out, (int(round(c.cx)), int(round(c.cy))), 2, (0, 255, 255), -1)
    return cv2.cvtColor(out, cv2.COLOR_BGR2RGB)


def make_contact_sheet(items: Sequence[ImageResult], out_path: Path, title: str, cols: int = 8, rows: int = 5) -> None:
    selected = list(items)[: cols * rows]
    if not selected:
        return
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 1.6, rows * 1.8))
    axes = np.atleast_2d(axes)
    for ax in axes.ravel():
        ax.axis("off")
    for ax, item in zip(axes.ravel(), selected):
        gray = cv2.imread(str(item.path), cv2.IMREAD_GRAYSCALE)
        ax.imshow(gray, cmap="gray", vmin=0, vmax=255)
        suffix = f"{item.prefix} {item.time_text}"
        if item.candidates:
            best = max(item.candidates, key=lambda c: c.score)
            ax.add_patch(plt.Rectangle((best.x, best.y), best.w, best.h, fill=False, edgecolor="red", linewidth=1.2))
            suffix += f" S={best.score:.0f}"
        ax.set_title(suffix, fontsize=7)
    fig.suptitle(title)
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def write_csv(results: Sequence[ImageResult], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "file", "prefix", "day", "time", "pano_x", "pano_y", "detected",
            "candidate_count", "best_cx", "best_cy", "best_area", "best_score",
            "best_contrast", "response_mean", "response_std", "response_max", "threshold",
        ])
        for r in results:
            best: Optional[Candidate] = max(r.candidates, key=lambda c: c.score) if r.candidates else None
            writer.writerow([
                r.path.name, r.prefix, r.day, r.time_text, r.pano_x, r.pano_y,
                int(bool(r.candidates)), len(r.candidates),
                f"{best.cx:.2f}" if best else "",
                f"{best.cy:.2f}" if best else "",
                best.area if best else "",
                f"{best.score:.3f}" if best else "",
                f"{best.contrast:.3f}" if best else "",
                f"{r.response_mean:.3f}",
                f"{r.response_std:.3f}",
                f"{r.response_max:.3f}",
                f"{r.threshold:.3f}",
            ])


def write_summary(summary: dict, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        f"images,{summary['images']}",
        f"detected_images,{summary['detected_images']}",
        f"total_candidates,{summary['total_candidates']}",
    ]
    for prefix in sorted(summary["prefix_counts"]):
        total = summary["prefix_counts"].get(prefix, 0)
        detected = summary["detected_counts"].get(prefix, 0)
        rate = detected / total if total else 0.0
        lines.append(f"prefix_{prefix}_total,{total}")
        lines.append(f"prefix_{prefix}_detected,{detected}")
        lines.append(f"prefix_{prefix}_detected_rate,{rate:.4f}")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def plot_prefix_counts(summary: dict, out_path: Path) -> None:
    prefixes = sorted(summary["prefix_counts"])
    totals = [summary["prefix_counts"][p] for p in prefixes]
    detected = [summary["detected_counts"].get(p, 0) for p in prefixes]
    x = np.arange(len(prefixes))
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    ax.bar(x - 0.18, totals, width=0.36, label="all samples")
    ax.bar(x + 0.18, detected, width=0.36, label="detected samples")
    ax.set_xticks(x, prefixes)
    ax.set_ylabel("image count")
    ax.set_title("20161018-4 prefix statistics")
    ax.legend()
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_score_distribution(results: Sequence[ImageResult], out_path: Path) -> None:
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    for prefix in sorted({r.prefix for r in results}):
        scores = [max(r.candidates, key=lambda c: c.score).score for r in results if r.prefix == prefix and r.candidates]
        if scores:
            ax.hist(scores, bins=32, alpha=0.55, label=prefix)
    ax.set_title("Best candidate score distribution")
    ax.set_xlabel("best score")
    ax.set_ylabel("image count")
    ax.legend()
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_candidate_centers(results: Sequence[ImageResult], out_path: Path) -> None:
    xs = []
    ys = []
    weights = []
    for r in results:
        if not r.candidates:
            continue
        best = max(r.candidates, key=lambda c: c.score)
        xs.append(best.cx)
        ys.append(best.cy)
        weights.append(best.score)
    fig, ax = plt.subplots(figsize=(5.6, 5.2))
    if xs:
        sc = ax.scatter(xs, ys, c=weights, s=14, cmap="magma", alpha=0.72)
        fig.colorbar(sc, ax=ax, label="score")
    ax.set_xlim(0, 256)
    ax.set_ylim(256, 0)
    ax.set_aspect("equal")
    ax.set_title("Detected candidate center distribution")
    ax.set_xlabel("crop x")
    ax.set_ylabel("crop y")
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def make_pipeline_figure(item: ImageResult, out_path: Path) -> None:
    gray = cv2.imread(str(item.path), cv2.IMREAD_GRAYSCALE)
    cands, meta = detect(gray)
    panels = [
        ("original", gray),
        ("median", meta["smooth"]),
        ("bright/dark response", meta["response"]),
        ("binary mask", meta["binary"]),
        ("candidates", draw_boxed(gray, cands)),
    ]
    fig, axes = plt.subplots(1, len(panels), figsize=(13, 2.8))
    for ax, (title, img) in zip(axes, panels):
        if img.ndim == 2:
            ax.imshow(img, cmap="gray")
        else:
            ax.imshow(img)
        ax.set_title(title, fontsize=9)
        ax.axis("off")
    fig.suptitle(item.path.name)
    fig.tight_layout()
    fig.savefig(out_path, dpi=200)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--fig-dir", type=Path, default=DEFAULT_FIG_DIR)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    args = parser.parse_args()

    paths = list_images(args.src)
    results, summary = evaluate(paths)

    write_csv(results, args.data_dir / "traditional_20161018_results.csv")
    write_summary(summary, args.data_dir / "traditional_20161018_summary.csv")

    by_prefix = sorted(results, key=lambda r: (r.prefix, r.time_text, r.path.name))
    detected = sorted([r for r in results if r.candidates], key=lambda r: max(c.score for c in r.candidates), reverse=True)
    undetected = [r for r in results if not r.candidates]

    make_contact_sheet(by_prefix, args.fig_dir / "dataset_20161018_samples.png", "20161018-4 sample crops")
    make_contact_sheet(detected, args.fig_dir / "traditional_detection_top.png", "Top traditional detections")
    make_contact_sheet(undetected, args.fig_dir / "traditional_detection_low_response.png", "Low-response / background-like samples")
    plot_prefix_counts(summary, args.fig_dir / "dataset_20161018_prefix_counts.png")
    plot_score_distribution(results, args.fig_dir / "traditional_score_distribution.png")
    plot_candidate_centers(results, args.fig_dir / "traditional_candidate_centers.png")
    if detected:
        make_pipeline_figure(detected[0], args.fig_dir / "traditional_detection_pipeline.png")

    print(f"images={summary['images']}")
    print(f"detected_images={summary['detected_images']}")
    print(f"total_candidates={summary['total_candidates']}")
    for prefix in sorted(summary["prefix_counts"]):
        total = summary["prefix_counts"][prefix]
        det = summary["detected_counts"].get(prefix, 0)
        print(f"{prefix}: total={total} detected={det} rate={det / total if total else 0:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
