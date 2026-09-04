#!/usr/bin/env python3
"""Export a YOLOv26s seed dataset from 20161018-4 traditional detections."""

from __future__ import annotations

import argparse
import csv
import random
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence, Tuple

import cv2
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from evaluate_traditional_20161018 import DEFAULT_SOURCE, Candidate, ImageResult, evaluate, list_images


DEFAULT_OUT = Path("/home/sht/work/DMX_qt/latex/data/progress_report/yolo26s_seed_20161018_native256")
DEFAULT_FIG = Path("/home/sht/work/DMX_qt/latex/figures/progress_report/yolo26s_seed_native256_samples.png")


@dataclass
class ExportItem:
    result: ImageResult
    split: str
    is_positive: bool
    image_rel: str
    label_rel: str


def expanded_box(c: Candidate, src_size: int, dst_size: int, min_side: int = 12) -> Tuple[float, float, float, float]:
    pad = (dst_size - src_size) / 2.0
    x0 = float(c.x)
    y0 = float(c.y)
    x1 = float(c.x + c.w)
    y1 = float(c.y + c.h)

    if x1 - x0 < min_side:
        m = (min_side - (x1 - x0)) / 2.0
        x0 -= m
        x1 += m
    if y1 - y0 < min_side:
        m = (min_side - (y1 - y0)) / 2.0
        y0 -= m
        y1 += m

    x0 = max(0.0, min(float(src_size - 1), x0)) + pad
    x1 = max(1.0, min(float(src_size), x1)) + pad
    y0 = max(0.0, min(float(src_size - 1), y0)) + pad
    y1 = max(1.0, min(float(src_size), y1)) + pad

    cx = ((x0 + x1) / 2.0) / dst_size
    cy = ((y0 + y1) / 2.0) / dst_size
    bw = (x1 - x0) / dst_size
    bh = (y1 - y0) / dst_size
    return cx, cy, bw, bh


def make_canvas(gray: np.ndarray, dst_size: int = 256) -> np.ndarray:
    if gray.ndim != 2:
        gray = cv2.cvtColor(gray, cv2.COLOR_BGR2GRAY)
    src_h, src_w = gray.shape[:2]
    if src_h != 256 or src_w != 256:
        gray = cv2.resize(gray, (256, 256), interpolation=cv2.INTER_AREA)
    if dst_size == 256:
        return gray
    canvas = np.full((dst_size, dst_size), int(np.median(gray)), dtype=np.uint8)
    pad = (dst_size - 256) // 2
    canvas[pad:pad + 256, pad:pad + 256] = gray
    return canvas


def split_items(results: Sequence[ImageResult], seed: int, val_ratio: float, max_negatives: int | None) -> List[ExportItem]:
    rng = random.Random(seed)
    positives = [r for r in results if r.candidates]
    negatives = [r for r in results if not r.candidates]
    positives.sort(key=lambda r: max(c.score for c in r.candidates), reverse=True)
    rng.shuffle(negatives)
    if max_negatives is not None:
        negatives = negatives[:max_negatives]

    def assign_split(items: Sequence[ImageResult], is_positive: bool) -> List[ExportItem]:
        shuffled = list(items)
        rng.shuffle(shuffled)
        val_count = max(1, int(round(len(shuffled) * val_ratio))) if shuffled else 0
        out: List[ExportItem] = []
        for idx, result in enumerate(shuffled):
            split = "val" if idx < val_count else "train"
            stem = result.path.stem
            image_rel = f"images/{split}/{stem}.jpg"
            label_rel = f"labels/{split}/{stem}.txt"
            out.append(ExportItem(result, split, is_positive, image_rel, label_rel))
        return out

    return assign_split(positives, True) + assign_split(negatives, False)


def reset_dirs(out_root: Path) -> None:
    if out_root.exists():
        shutil.rmtree(out_root)
    for rel in ("images/train", "images/val", "labels/train", "labels/val"):
        (out_root / rel).mkdir(parents=True, exist_ok=True)


def write_dataset(items: Sequence[ExportItem], out_root: Path, image_size: int) -> None:
    reset_dirs(out_root)
    manifest_path = out_root / "manifest.csv"
    with manifest_path.open("w", encoding="utf-8", newline="") as mf:
        writer = csv.writer(mf)
        writer.writerow([
            "split", "image", "label", "source", "prefix", "time", "pano_x", "pano_y",
            "positive", "candidate_count", "best_score",
        ])
        for item in items:
            gray = cv2.imread(str(item.result.path), cv2.IMREAD_GRAYSCALE)
            if gray is None:
                continue
            canvas = make_canvas(gray, image_size)
            image_path = out_root / item.image_rel
            label_path = out_root / item.label_rel
            image_path.parent.mkdir(parents=True, exist_ok=True)
            label_path.parent.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(image_path), canvas, [int(cv2.IMWRITE_JPEG_QUALITY), 95])

            lines = []
            if item.is_positive:
                for cand in item.result.candidates:
                    cx, cy, bw, bh = expanded_box(cand, 256, image_size)
                    lines.append(f"0 {cx:.6f} {cy:.6f} {bw:.6f} {bh:.6f}")
            label_path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
            best = max(item.result.candidates, key=lambda c: c.score) if item.result.candidates else None
            writer.writerow([
                item.split,
                item.image_rel,
                item.label_rel,
                str(item.result.path),
                item.result.prefix,
                item.result.time_text,
                item.result.pano_x,
                item.result.pano_y,
                int(item.is_positive),
                len(item.result.candidates),
                f"{best.score:.3f}" if best else "",
            ])

    (out_root / "data.yaml").write_text(
        "\n".join([
            f"path: {out_root}",
            "train: images/train",
            "val: images/val",
            "names:",
            "  0: flying_object",
            "",
        ]),
        encoding="utf-8",
    )
    (out_root / "README.md").write_text(
        "# YOLOv26s seed dataset: 20161018-4\n\n"
        f"Images are exported as {image_size}x{image_size} grayscale JPG files.\n"
        "Labels are pseudo labels generated by the traditional detector and should be manually reviewed before final training.\n",
        encoding="utf-8",
    )


def write_summary(items: Sequence[ExportItem], out_root: Path) -> None:
    rows = []
    for split in ("train", "val"):
        split_items_ = [i for i in items if i.split == split]
        pos = [i for i in split_items_ if i.is_positive]
        neg = [i for i in split_items_ if not i.is_positive]
        rows.append((split, len(split_items_), len(pos), len(neg), sum(len(i.result.candidates) for i in pos)))
    with (out_root / "summary.csv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["split", "images", "positive_images", "negative_images", "boxes"])
        writer.writerows(rows)


def draw_visual(items: Sequence[ExportItem], fig_path: Path, image_size: int) -> None:
    positives = [i for i in items if i.is_positive]
    positives.sort(key=lambda i: max(c.score for c in i.result.candidates), reverse=True)
    selected = positives[:24]
    cols = 6
    rows = 4
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 2.0, rows * 2.15))
    for ax in axes.ravel():
        ax.axis("off")
    for ax, item in zip(axes.ravel(), selected):
        gray = cv2.imread(str(item.result.path), cv2.IMREAD_GRAYSCALE)
        canvas = make_canvas(gray, image_size)
        ax.imshow(canvas, cmap="gray", vmin=0, vmax=255)
        for cand in item.result.candidates:
            cx, cy, bw, bh = expanded_box(cand, 256, image_size)
            x = (cx - bw / 2.0) * image_size
            y = (cy - bh / 2.0) * image_size
            ax.add_patch(plt.Rectangle((x, y), bw * image_size, bh * image_size, fill=False, edgecolor="red", linewidth=1.0))
        ax.set_title(f"{item.result.prefix} {item.result.time_text}", fontsize=7)
    fig.suptitle(f"YOLOv26s seed positives ({image_size}x{image_size}) with pseudo boxes")
    fig.tight_layout()
    fig_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(fig_path, dpi=180)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--fig", type=Path, default=DEFAULT_FIG)
    parser.add_argument("--seed", type=int, default=20260701)
    parser.add_argument("--val-ratio", type=float, default=0.2)
    parser.add_argument("--max-negatives", type=int, default=None)
    parser.add_argument("--image-size", type=int, default=256, choices=[256, 320, 640])
    args = parser.parse_args()

    results, _summary = evaluate(list_images(args.src))
    positives = sum(1 for r in results if r.candidates)
    max_negatives = args.max_negatives if args.max_negatives is not None else positives
    items = split_items(results, args.seed, args.val_ratio, max_negatives)
    write_dataset(items, args.out, args.image_size)
    write_summary(items, args.out)
    draw_visual(items, args.fig, args.image_size)

    print(f"out={args.out}")
    print(f"images={len(items)}")
    print(f"positive_images={sum(1 for i in items if i.is_positive)}")
    print(f"negative_images={sum(1 for i in items if not i.is_positive)}")
    print(f"boxes={sum(len(i.result.candidates) for i in items if i.is_positive)}")
    print(f"fig={args.fig}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
