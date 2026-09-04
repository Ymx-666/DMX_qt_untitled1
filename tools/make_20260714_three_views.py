#!/usr/bin/env python3
"""Generate three AB-view analysis images for the 20260714 BW recording.

Outputs are written to:

    /mnt/dmx4t/DMX_yangben/20260714/_analysis/traditional

The three main AB images are preview panoramas with A on top and B below:

1. sky mask overlay on the original image
2. YOLOv26s filtered detections on the original image
3. current traditional filtered detections on the original image

A/B full-resolution split images and candidate CSVs are also saved for detail
inspection. A and B are processed independently; they are only stacked for
visualization after processing.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import cv2
import numpy as np


SOURCE_DIR = Path("/mnt/dmx4t/data/recordings/20260714/bw/17")
ANALYSIS_DIR = Path("/mnt/dmx4t/DMX_yangben/20260714/_analysis")
OUT_DIR = ANALYSIS_DIR / "traditional"
STEM = "BW_20260714_170002_2366"
YOLO_MODEL = Path("/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights/best.onnx")


@dataclass
class Box:
    stream: str
    detector: str
    cls: str
    score: float
    adjusted_score: float
    x1: int
    y1: int
    x2: int
    y2: int
    cx: int
    cy: int
    source: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=SOURCE_DIR)
    parser.add_argument("--analysis-dir", type=Path, default=ANALYSIS_DIR)
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR)
    parser.add_argument("--stem", default=STEM)
    parser.add_argument("--yolo-model", type=Path, default=YOLO_MODEL)
    parser.add_argument("--ab-width", type=int, default=8192)
    parser.add_argument("--yolo-raw-threshold", type=float, default=0.0003)
    parser.add_argument("--yolo-keep-per-stream", type=int, default=12)
    parser.add_argument("--clean", action="store_true", default=True)
    return parser.parse_args()


def clean_output_dir(out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for path in out_dir.iterdir():
        if path.is_file() or path.is_symlink():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)


def stream_image_path(source_dir: Path, stem: str, stream: str) -> Path:
    return source_dir / f"{stem}-{stream}.jpg"


def list_stream_frames(source_dir: Path, stream: str, limit: int) -> List[Path]:
    pattern = re.compile(r"^BW_\d{8}_\d{6}_\d+-%s\.jpg$" % re.escape(stream))
    frames = [p for p in source_dir.glob("*.jpg") if pattern.match(p.name)]
    frames.sort()
    return frames[:limit]


def load_gray(path: Path) -> np.ndarray:
    gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if gray is None:
        raise RuntimeError(f"failed to read image: {path}")
    return gray


def build_background(source_dir: Path, stream: str, frames: int = 3) -> np.ndarray:
    paths = list_stream_frames(source_dir, stream, frames)
    if len(paths) < frames:
        raise RuntimeError(f"not enough {stream} frames for background in {source_dir}")
    accum = None
    for path in paths:
        gray = load_gray(path).astype(np.float32)
        if accum is None:
            accum = np.zeros_like(gray)
        accum += gray
    assert accum is not None
    return np.clip(accum / float(len(paths)), 0, 255).astype(np.uint8)


def resize_for_mask(gray: np.ndarray, scale: float = 0.25) -> np.ndarray:
    return cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)


def fill_component_holes(mask: np.ndarray) -> np.ndarray:
    flood = mask.copy()
    h, w = flood.shape
    canvas = np.zeros((h + 2, w + 2), np.uint8)
    cv2.floodFill(flood, canvas, (0, 0), 255)
    holes = cv2.bitwise_not(flood)
    return cv2.bitwise_or(mask, holes)


def build_sky_mask(background: np.ndarray) -> np.ndarray:
    """Build a multi-component sky mask at full image resolution."""
    small = resize_for_mask(background, 0.25)
    h, w = small.shape
    blur = cv2.GaussianBlur(small, (0, 0), 3.0)
    grad_x = cv2.Sobel(blur, cv2.CV_32F, 1, 0, 3)
    grad_y = cv2.Sobel(blur, cv2.CV_32F, 0, 1, 3)
    grad = cv2.magnitude(grad_x, grad_y)
    local_mean = cv2.blur(blur.astype(np.float32), (17, 17))
    local_sq = cv2.blur((blur.astype(np.float32) ** 2), (17, 17))
    texture = np.sqrt(np.maximum(local_sq - local_mean ** 2, 0.0))

    top = blur[: max(1, int(h * 0.55)), :]
    # Thermal BW sky in this sample is a smooth mid/high-gray region. The older
    # percentile floor was too permissive on A and swallowed low-texture building
    # and tree regions. Cap the adaptive floor so bright saturated B sky is still
    # retained, but keep A's dark buildings/trees out.
    bright_floor = min(70.0, max(50.0, float(np.percentile(top, 65))))
    grad_limit = min(14.0, max(6.0, float(np.percentile(grad[: max(1, int(h * 0.75)), :], 58))))
    tex_limit = min(7.0, max(2.5, float(np.percentile(texture[: max(1, int(h * 0.75)), :], 62))))

    yy = np.arange(h, dtype=np.float32)[:, None]
    upper_weight = yy < h * 0.74
    candidate = (
        (blur.astype(np.float32) >= bright_floor)
        & (grad <= max(8.0, grad_limit))
        & (texture <= max(5.0, tex_limit))
        & upper_weight
    )

    mask = (candidate.astype(np.uint8) * 255)
    close_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (21, 21))
    open_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, close_kernel)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, open_kernel)
    mask = fill_component_holes(mask)

    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    kept = np.zeros_like(mask)
    min_area = max(2000, int(mask.size * 0.002))
    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        top_y = int(stats[lab, cv2.CC_STAT_TOP])
        if area < min_area:
            continue
        if top_y > int(h * 0.70):
            continue
        kept[labels == lab] = 255

    # Remove a thin band of mixed high-gradient vegetation/building edge pixels.
    edge_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    kept = cv2.erode(kept, edge_kernel, iterations=1)
    full = cv2.resize(kept, (background.shape[1], background.shape[0]), interpolation=cv2.INTER_NEAREST)
    return full


def overlay_sky(gray: np.ndarray, mask: np.ndarray) -> np.ndarray:
    out = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    idx = mask > 0
    # Cyan-blue overlay on sky while preserving the original luminance texture.
    out[idx, 0] = np.clip(out[idx, 0].astype(np.float32) * 0.55 + 255.0 * 0.45, 0, 255).astype(np.uint8)
    out[idx, 1] = np.clip(out[idx, 1].astype(np.float32) * 0.55 + 180.0 * 0.45, 0, 255).astype(np.uint8)
    out[idx, 2] = np.clip(out[idx, 2].astype(np.float32) * 0.55 + 30.0 * 0.45, 0, 255).astype(np.uint8)
    return out


def write_csv(path: Path, rows: Sequence[dict], fields: Sequence[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(fields))
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def generate_windows(mask: np.ndarray, window: int = 512, step: int = 256) -> List[Tuple[int, int]]:
    small = cv2.resize(mask, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_NEAREST)
    count, labels, stats, _ = cv2.connectedComponentsWithStats((small > 0).astype(np.uint8), 8)
    h, w = mask.shape
    windows: List[Tuple[int, int]] = []
    seen = set()

    def add_window(x: int, y: int) -> None:
        x = int(max(0, min(w - window, x))) if w >= window else int((w - window) // 2)
        y = int(max(0, min(h - window, y))) if h >= window else int((h - window) // 2)
        key = (x, y)
        if key in seen:
            return
        seen.add(key)
        windows.append(key)

    for lab in range(1, count):
        area = int(stats[lab, cv2.CC_STAT_AREA])
        if area < 50:
            continue
        sx = int(stats[lab, cv2.CC_STAT_LEFT] * 4)
        sy = int(stats[lab, cv2.CC_STAT_TOP] * 4)
        sw = int(stats[lab, cv2.CC_STAT_WIDTH] * 4)
        sh = int(stats[lab, cv2.CC_STAT_HEIGHT] * 4)
        x0 = max(0, sx - 32)
        y0 = max(0, sy - 32)
        x1 = min(w, sx + sw + 32)
        y1 = min(h, sy + sh + 32)
        if x1 <= x0 or y1 <= y0:
            continue
        if x1 - x0 <= window:
            xs = [int((x0 + x1 - window) / 2)]
        else:
            xs = list(range(x0, max(x0, x1 - window) + 1, step))
            xs.append(x1 - window)
        if y1 - y0 <= window:
            ys = [int((y0 + y1 - window) / 2)]
        else:
            ys = list(range(y0, max(y0, y1 - window) + 1, step))
            ys.append(y1 - window)
        for y in ys:
            for x in xs:
                add_window(x, y)

    filtered: List[Tuple[int, int]] = []
    for x, y in windows:
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(w, x + window), min(h, y + window)
        roi = mask[y0:y1, x0:x1]
        if roi.size and float(np.mean(roi > 0)) >= 0.18:
            filtered.append((x, y))
    return filtered


def crop_with_replicate(gray: np.ndarray, x: int, y: int, size: int) -> np.ndarray:
    h, w = gray.shape
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(w, x + size)
    y1 = min(h, y + size)
    crop = gray[y0:y1, x0:x1]
    if crop.shape == (size, size):
        return crop.copy()
    left = max(0, -x)
    top = max(0, -y)
    right = max(0, x + size - w)
    bottom = max(0, y + size - h)
    return cv2.copyMakeBorder(crop, top, bottom, left, right, cv2.BORDER_REPLICATE)


def preprocess_yolo_window(gray: np.ndarray, mask: np.ndarray, x: int, y: int, size: int = 512) -> np.ndarray:
    crop = crop_with_replicate(gray, x, y, size)
    mask_crop = crop_with_replicate(mask, x, y, size)
    sky_values = crop[mask_crop > 0]
    fill = int(np.median(sky_values)) if sky_values.size else int(np.median(crop))
    crop = crop.copy()
    crop[mask_crop == 0] = fill

    clahe = cv2.createCLAHE(clipLimit=1.8, tileGridSize=(8, 8))
    eq = clahe.apply(crop)
    blur = cv2.GaussianBlur(eq, (0, 0), 1.2)
    sharp = cv2.addWeighted(eq, 1.45, blur, -0.45, 0)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
    blackhat = cv2.morphologyEx(sharp, cv2.MORPH_BLACKHAT, kernel)
    enhanced = cv2.addWeighted(sharp, 1.0, blackhat, 0.55, 0)
    return cv2.cvtColor(enhanced, cv2.COLOR_GRAY2BGR)


def parse_yolo_outputs(outs: Sequence[np.ndarray],
                       input_size: int,
                       win_size: int,
                       threshold: float) -> List[Tuple[int, int, int, int, int, float]]:
    detections: List[Tuple[int, int, int, int, int, float]] = []

    def parse_row(row: np.ndarray) -> None:
        attrs = int(row.shape[0])
        if attrs < 6:
            return
        class_start = 4
        scores = row[class_start:]
        cls = int(np.argmax(scores))
        conf = float(scores[cls])
        if conf < threshold:
            return
        cx, cy, bw, bh = [float(v) for v in row[:4]]
        max_coord = max(abs(cx), abs(cy), abs(bw), abs(bh))
        if max_coord <= 2.0:
            cx *= input_size
            cy *= input_size
            bw *= input_size
            bh *= input_size
        if bw <= 1.0 or bh <= 1.0:
            return
        scale = float(win_size) / float(input_size)
        x1 = int(round((cx - bw * 0.5) * scale))
        y1 = int(round((cy - bh * 0.5) * scale))
        x2 = int(round((cx + bw * 0.5) * scale))
        y2 = int(round((cy + bh * 0.5) * scale))
        x1 = max(0, min(win_size - 1, x1))
        y1 = max(0, min(win_size - 1, y1))
        x2 = max(0, min(win_size - 1, x2))
        y2 = max(0, min(win_size - 1, y2))
        if x2 <= x1 or y2 <= y1:
            return
        detections.append((x1, y1, x2, y2, cls, conf))

    for out in outs:
        arr = np.asarray(out)
        if arr.dtype != np.float32:
            arr = arr.astype(np.float32)
        if arr.ndim == 3:
            arr = arr[0]
        if arr.ndim != 2:
            continue
        # Common YOLO export for this model is (attrs, anchors): (6, 8400).
        if arr.shape[0] <= 256 and arr.shape[1] > arr.shape[0]:
            for i in range(arr.shape[1]):
                parse_row(arr[:, i])
        else:
            for i in range(arr.shape[0]):
                parse_row(arr[i, :])
    return detections


def iou(a: Box, b: Box) -> float:
    ix1 = max(a.x1, b.x1)
    iy1 = max(a.y1, b.y1)
    ix2 = min(a.x2, b.x2)
    iy2 = min(a.y2, b.y2)
    iw = max(0, ix2 - ix1)
    ih = max(0, iy2 - iy1)
    inter = iw * ih
    if inter <= 0:
        return 0.0
    area_a = max(1, (a.x2 - a.x1) * (a.y2 - a.y1))
    area_b = max(1, (b.x2 - b.x1) * (b.y2 - b.y1))
    return float(inter) / float(area_a + area_b - inter)


def nms_boxes(boxes: Sequence[Box], iou_threshold: float = 0.35) -> List[Box]:
    kept: List[Box] = []
    for box in sorted(boxes, key=lambda item: item.adjusted_score, reverse=True):
        if all(iou(box, old) <= iou_threshold for old in kept):
            kept.append(box)
    return kept


def run_yolo_for_stream(stream: str,
                        gray: np.ndarray,
                        mask: np.ndarray,
                        net: cv2.dnn.Net,
                        raw_threshold: float,
                        keep_per_stream: int) -> Tuple[List[Box], int, int]:
    input_size = 640
    win_size = 512
    windows = generate_windows(mask, win_size, 256)
    dist = cv2.distanceTransform((mask > 0).astype(np.uint8), cv2.DIST_L2, 3)
    boxes: List[Box] = []
    class_names = ["drone", "bird"]

    for wx, wy in windows:
        crop_bgr = preprocess_yolo_window(gray, mask, wx, wy, win_size)
        resized = cv2.resize(crop_bgr, (input_size, input_size), interpolation=cv2.INTER_LINEAR)
        blob = cv2.dnn.blobFromImage(resized, 1.0 / 255.0, (input_size, input_size), (0, 0, 0), True, False)
        try:
            net.setInput(blob)
            outs = net.forward(net.getUnconnectedOutLayersNames())
        except cv2.error:
            continue
        local_detections = parse_yolo_outputs(outs, input_size, win_size, raw_threshold)
        for x1, y1, x2, y2, cls, raw in local_detections:
            gx1 = max(0, min(gray.shape[1] - 1, wx + x1))
            gy1 = max(0, min(gray.shape[0] - 1, wy + y1))
            gx2 = max(0, min(gray.shape[1] - 1, wx + x2))
            gy2 = max(0, min(gray.shape[0] - 1, wy + y2))
            if gx2 <= gx1 or gy2 <= gy1:
                continue
            cx = int((gx1 + gx2) / 2)
            cy = int((gy1 + gy2) / 2)
            if mask[cy, cx] == 0:
                continue
            bw = gx2 - gx1
            bh = gy2 - gy1
            if bw > 220 or bh > 220 or bw < 2 or bh < 2:
                continue
            edge = float(dist[cy, cx])
            edge_weight = min(1.0, max(0.25, edge / 80.0))
            roi = gray[max(0, cy - 16): min(gray.shape[0], cy + 17), max(0, cx - 16): min(gray.shape[1], cx + 17)]
            local_std = float(np.std(roi)) if roi.size else 0.0
            exposure_weight = 0.70 if (roi.size and float(np.mean(roi)) > 238.0 and local_std < 3.0) else 1.0
            adjusted = raw * edge_weight * exposure_weight
            boxes.append(
                Box(
                    stream=stream,
                    detector="yolov26s",
                    cls=class_names[cls] if 0 <= cls < len(class_names) else f"class_{cls}",
                    score=raw,
                    adjusted_score=adjusted,
                    x1=gx1,
                    y1=gy1,
                    x2=gx2,
                    y2=gy2,
                    cx=cx,
                    cy=cy,
                    source=f"win({wx},{wy})",
                )
            )

    kept = nms_boxes(boxes, 0.35)
    kept = [b for b in kept if b.adjusted_score >= raw_threshold * 0.65]
    kept = kept[:keep_per_stream]
    return kept, len(windows), len(boxes)


def read_traditional_filtered(analysis_dir: Path, stem: str) -> List[Box]:
    decisions = analysis_dir / "traditional_fp_reduction_decisions.csv"
    if not decisions.is_file():
        raise RuntimeError(f"missing traditional decision CSV: {decisions}")
    boxes: List[Box] = []
    with decisions.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("decision") != "accept":
                continue
            stream = row["stream"]
            x1 = int(float(row["x1"]))
            y1 = int(float(row["y1"]))
            x2 = int(float(row["x2"]))
            y2 = int(float(row["y2"]))
            cx = int(float(row["x"]))
            cy = int(float(row["y"]))
            score = float(row["score"])
            boxes.append(
                Box(
                    stream=stream,
                    detector="traditional",
                    cls="point_target",
                    score=score,
                    adjusted_score=score,
                    x1=x1,
                    y1=y1,
                    x2=x2,
                    y2=y2,
                    cx=cx,
                    cy=cy,
                    source=f"idx={row['idx']} {row.get('reason', '')}",
                )
            )
    return boxes


def draw_boxes(gray: np.ndarray,
               boxes: Sequence[Box],
               title: str,
               color: Tuple[int, int, int]) -> np.ndarray:
    out = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    for i, box in enumerate(boxes, 1):
        pad = 14 if box.detector == "traditional" else 8
        x1 = max(0, box.x1 - pad)
        y1 = max(0, box.y1 - pad)
        x2 = min(out.shape[1] - 1, box.x2 + pad)
        y2 = min(out.shape[0] - 1, box.y2 + pad)
        thickness = 5 if out.shape[1] > 10000 else 2
        cv2.rectangle(out, (x1, y1), (x2, y2), color, thickness)
        label = f"{i}:{box.cls} {box.adjusted_score:.4g}"
        cv2.putText(out, label, (max(0, x1 - 10), max(42, y1 - 18)),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.0, color, 3, cv2.LINE_AA)
    cv2.putText(out, f"{title} kept={len(boxes)}", (40, 80),
                cv2.FONT_HERSHEY_SIMPLEX, 1.8, color, 5, cv2.LINE_AA)
    return out


def save_image(path: Path, image: np.ndarray, quality: int = 92) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(path), image, [int(cv2.IMWRITE_JPEG_QUALITY), int(quality)])


def make_ab_preview(a_path: Path, b_path: Path, out_path: Path, width: int) -> None:
    panels: List[np.ndarray] = []
    for label, path in (("A", a_path), ("B", b_path)):
        img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if img is None:
            raise RuntimeError(f"failed to read output image: {path}")
        scale = width / float(img.shape[1])
        small = cv2.resize(img, (width, max(1, int(img.shape[0] * scale))), interpolation=cv2.INTER_AREA)
        cv2.putText(small, label, (30, 90), cv2.FONT_HERSHEY_SIMPLEX, 3.0, (0, 0, 255), 8, cv2.LINE_AA)
        panels.append(small)
    save_image(out_path, np.vstack(panels), quality=94)


def write_boxes_csv(path: Path, boxes: Sequence[Box]) -> None:
    fields = ["stream", "detector", "class", "score", "adjusted_score", "x1", "y1", "x2", "y2", "cx", "cy", "source"]
    rows = [
        {
            "stream": b.stream,
            "detector": b.detector,
            "class": b.cls,
            "score": f"{b.score:.8g}",
            "adjusted_score": f"{b.adjusted_score:.8g}",
            "x1": b.x1,
            "y1": b.y1,
            "x2": b.x2,
            "y2": b.y2,
            "cx": b.cx,
            "cy": b.cy,
            "source": b.source,
        }
        for b in boxes
    ]
    write_csv(path, rows, fields)


def main() -> int:
    args = parse_args()
    if args.clean:
        clean_output_dir(args.out_dir)
    else:
        args.out_dir.mkdir(parents=True, exist_ok=True)

    cv2.setNumThreads(max(1, cv2.getNumThreads()))

    masks: Dict[str, np.ndarray] = {}
    grays: Dict[str, np.ndarray] = {}
    sky_rows: List[dict] = []

    for stream in ("A", "B"):
        image_path = stream_image_path(args.source_dir, args.stem, stream)
        gray = load_gray(image_path)
        bg = build_background(args.source_dir, stream, 3)
        mask = build_sky_mask(bg)
        grays[stream] = gray
        masks[stream] = mask
        sky_rows.append({
            "stream": stream,
            "image": image_path.name,
            "sky_pixels": int(np.count_nonzero(mask)),
            "total_pixels": int(mask.size),
            "sky_ratio": f"{float(np.count_nonzero(mask)) / float(mask.size):.6f}",
        })
        overlay = overlay_sky(gray, mask)
        save_image(args.out_dir / f"01_{stream}_sky_mask_on_original_full.jpg", overlay, 92)
        cv2.imwrite(str(args.out_dir / f"01_{stream}_sky_mask.png"), mask)

    make_ab_preview(
        args.out_dir / "01_A_sky_mask_on_original_full.jpg",
        args.out_dir / "01_B_sky_mask_on_original_full.jpg",
        args.out_dir / "01_AB_sky_mask_on_original.jpg",
        args.ab_width,
    )
    write_csv(args.out_dir / "01_sky_mask_stats.csv", sky_rows, ["stream", "image", "sky_pixels", "total_pixels", "sky_ratio"])

    if not args.yolo_model.is_file():
        raise RuntimeError(f"YOLO model not found: {args.yolo_model}")
    net = cv2.dnn.readNetFromONNX(str(args.yolo_model))
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

    yolo_boxes: List[Box] = []
    yolo_summary: List[dict] = []
    for stream in ("A", "B"):
        boxes, window_count, raw_count = run_yolo_for_stream(
            stream,
            grays[stream],
            masks[stream],
            net,
            args.yolo_raw_threshold,
            args.yolo_keep_per_stream,
        )
        yolo_boxes.extend(boxes)
        yolo_summary.append({
            "stream": stream,
            "windows": window_count,
            "raw_candidates": raw_count,
            "kept": len(boxes),
        })
        vis = draw_boxes(grays[stream], boxes, f"{stream} YOLOv26s filtered", (0, 255, 255))
        save_image(args.out_dir / f"02_{stream}_yolov26s_filtered_on_original_full.jpg", vis, 92)

    make_ab_preview(
        args.out_dir / "02_A_yolov26s_filtered_on_original_full.jpg",
        args.out_dir / "02_B_yolov26s_filtered_on_original_full.jpg",
        args.out_dir / "02_AB_yolov26s_filtered_on_original.jpg",
        args.ab_width,
    )
    write_boxes_csv(args.out_dir / "02_yolov26s_filtered_candidates.csv", yolo_boxes)
    write_csv(args.out_dir / "02_yolov26s_summary.csv", yolo_summary, ["stream", "windows", "raw_candidates", "kept"])

    traditional_boxes = read_traditional_filtered(args.analysis_dir, args.stem)
    for stream in ("A", "B"):
        boxes = [box for box in traditional_boxes if box.stream == stream]
        vis = draw_boxes(grays[stream], boxes, f"{stream} traditional filtered", (0, 0, 255))
        save_image(args.out_dir / f"03_{stream}_traditional_filtered_on_original_full.jpg", vis, 92)

    make_ab_preview(
        args.out_dir / "03_A_traditional_filtered_on_original_full.jpg",
        args.out_dir / "03_B_traditional_filtered_on_original_full.jpg",
        args.out_dir / "03_AB_traditional_filtered_on_original.jpg",
        args.ab_width,
    )
    write_boxes_csv(args.out_dir / "03_traditional_filtered_candidates.csv", traditional_boxes)

    print(f"out_dir={args.out_dir}")
    print("sky:", sky_rows)
    print("yolo:", yolo_summary)
    print(f"traditional kept={len(traditional_boxes)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
