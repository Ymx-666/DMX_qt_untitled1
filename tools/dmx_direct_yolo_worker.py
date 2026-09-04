#!/usr/bin/env python3
"""Persistent direct-YOLO worker for the DMX replay test build.

The worker receives JSONL frame requests on stdin and writes JSONL results to
stdout. It always runs a full-frame branch, then uses the remaining frame
budget for native-resolution sky windows. RGB and BW are processed as
independent discovery streams; low-confidence cross-stream observations can
corroborate each other later by panorama position.
"""

from __future__ import annotations

import argparse
import ast
import json
import math
import re
import sys
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import cv2
import numpy as np
import onnxruntime as ort


FRAME_TIME_RE = re.compile(r"_(\d{8})_(\d{6})_")


@dataclass
class Detection:
    x1: float
    y1: float
    x2: float
    y2: float
    confidence: float
    class_id: int
    class_name: str
    branch: str

    @property
    def cx(self) -> float:
        return (self.x1 + self.x2) * 0.5

    @property
    def cy(self) -> float:
        return (self.y1 + self.y2) * 0.5


@dataclass
class HistoryDetection:
    stream: str
    class_id: int
    pano_x: int
    pano_y: int
    confidence: float
    rx_ms: int
    wall_time: float


@dataclass
class StaticClutterTrack:
    track_id: int
    stream: str
    class_id: int
    pano_width: int
    anchor_x: int
    anchor_y: int
    reference_hash: int
    reference_area: float
    reference_aspect: float
    first_wall_time: float
    last_wall_time: float
    last_file_index: int
    stable_hits: int = 1
    context_hits: int = 0
    suppressed: bool = False


@dataclass
class StaticClutterDecision:
    track_id: int = 0
    stable_hits: int = 0
    context_hits: int = 0
    context_score: float = 0.0
    hash_distance: int = -1
    suppressed: bool = False
    newly_suppressed: bool = False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--save-root", type=Path, required=True)
    parser.add_argument("--input-size", type=int, default=512)
    parser.add_argument("--class-names", default="")
    parser.add_argument("--high-threshold", type=float, default=0.12)
    parser.add_argument("--low-threshold", type=float, default=0.05)
    parser.add_argument("--bird-ratio", type=float, default=1.10)
    parser.add_argument("--iou-threshold", type=float, default=0.45)
    parser.add_argument("--sky-coverage", type=float, default=0.15)
    parser.add_argument("--frame-budget-ms", type=float, default=240.0)
    parser.add_argument("--catchup-budget-ms", type=float, default=120.0)
    parser.add_argument("--max-queue-delay-ms", type=float, default=450.0)
    parser.add_argument("--max-output", type=int, default=3)
    parser.add_argument("--window-size", type=int, default=640)
    parser.add_argument("--window-stride", type=int, default=512)
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument("--static-clutter", action="store_true")
    parser.add_argument("--static-min-hits", type=int, default=5)
    parser.add_argument("--static-min-duration-ms", type=int, default=24000)
    parser.add_argument("--static-min-context-hits", type=int, default=3)
    parser.add_argument("--static-context-threshold", type=float, default=0.10)
    return parser.parse_args()


def emit_json(payload: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=True, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def rotate_for_panorama(image: np.ndarray) -> np.ndarray:
    rotated = cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return cv2.flip(rotated, 1)


def axis_starts(length: int, size: int, stride: int) -> list[int]:
    if length <= size:
        return [0]
    starts = list(range(0, length - size + 1, stride))
    last = length - size
    if not starts or starts[-1] != last:
        starts.append(last)
    return starts


def box_iou(a: Detection, b: Detection) -> float:
    x1 = max(a.x1, b.x1)
    y1 = max(a.y1, b.y1)
    x2 = min(a.x2, b.x2)
    y2 = min(a.y2, b.y2)
    intersection = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    if intersection <= 0.0:
        return 0.0
    area_a = max(0.0, a.x2 - a.x1) * max(0.0, a.y2 - a.y1)
    area_b = max(0.0, b.x2 - b.x1) * max(0.0, b.y2 - b.y1)
    return intersection / max(1e-9, area_a + area_b - intersection)


def non_max_suppression(
    detections: list[Detection],
    iou_threshold: float,
) -> list[Detection]:
    ranked = sorted(detections, key=lambda item: item.confidence, reverse=True)
    kept: list[Detection] = []
    for detection in ranked:
        duplicate = False
        for old in kept:
            center_distance = math.hypot(detection.cx - old.cx, detection.cy - old.cy)
            if box_iou(detection, old) >= iou_threshold or center_distance <= 24.0:
                duplicate = True
                break
        if not duplicate:
            kept.append(detection)
    return kept


def fixed_crop(
    image: np.ndarray,
    center_x: int,
    center_y: int,
    crop_size: int,
) -> tuple[np.ndarray, int, int]:
    origin_x = center_x - crop_size // 2
    origin_y = center_y - crop_size // 2
    result = np.zeros((crop_size, crop_size, 3), dtype=np.uint8)
    src_x1 = max(0, origin_x)
    src_y1 = max(0, origin_y)
    src_x2 = min(image.shape[1], origin_x + crop_size)
    src_y2 = min(image.shape[0], origin_y + crop_size)
    if src_x2 > src_x1 and src_y2 > src_y1:
        dst_x1 = src_x1 - origin_x
        dst_y1 = src_y1 - origin_y
        result[
            dst_y1 : dst_y1 + (src_y2 - src_y1),
            dst_x1 : dst_x1 + (src_x2 - src_x1),
        ] = image[src_y1:src_y2, src_x1:src_x2]
    return result, origin_x, origin_y


class StaticClutterVerifier:
    """Suppresses repeated world-fixed structures while preserving changed ROIs."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.enabled = bool(args.static_clutter)
        self.min_hits = max(3, int(args.static_min_hits))
        self.min_duration_ms = max(8000, int(args.static_min_duration_ms))
        self.min_context_hits = max(1, int(args.static_min_context_hits))
        self.context_threshold = max(0.01, float(args.static_context_threshold))
        self.tracks: list[StaticClutterTrack] = []
        self.next_track_id = 0

    @staticmethod
    def _positive_ratio(a: float, b: float) -> float:
        if a <= 0.0 or b <= 0.0:
            return 1.0
        return max(a, b) / max(1e-6, min(a, b))

    @staticmethod
    def _wrapped_distance(a: int, b: int, width: int) -> int:
        distance = abs(a - b)
        return min(distance, max(0, width - distance))

    @staticmethod
    def _appearance_hash(image: np.ndarray, detection: Detection) -> int:
        box_width = max(1, int(round(detection.x2 - detection.x1)))
        box_height = max(1, int(round(detection.y2 - detection.y1)))
        side = max(32, max(box_width, box_height) * 3)
        center_x = int(round(detection.cx))
        center_y = int(round(detection.cy))
        x1 = max(0, center_x - side // 2)
        y1 = max(0, center_y - side // 2)
        x2 = min(image.shape[1], center_x + (side + 1) // 2)
        y2 = min(image.shape[0], center_y + (side + 1) // 2)
        patch = image[y1:y2, x1:x2]
        if patch.size == 0:
            return 0
        gray = cv2.cvtColor(
            cv2.resize(patch, (9, 8), interpolation=cv2.INTER_LINEAR),
            cv2.COLOR_BGR2GRAY,
        )
        comparisons = (gray[:, :8] > gray[:, 1:]).reshape(-1)
        value = 0
        for bit, enabled in enumerate(comparisons):
            if enabled:
                value |= 1 << bit
        return value

    @staticmethod
    def _vertical_support_score(image: np.ndarray, detection: Detection) -> float:
        box_width = max(1, int(round(detection.x2 - detection.x1)))
        box_height = max(1, int(round(detection.y2 - detection.y1)))
        center_x = int(round(detection.cx))
        half_width = max(20, box_width // 3)
        x1 = max(0, center_x - half_width)
        x2 = min(image.shape[1], center_x + half_width + 1)
        y1 = max(0, min(image.shape[0] - 1, int(round(detection.y2))))
        y2 = min(image.shape[0], y1 + max(100, box_height * 6))
        if x2 - x1 < 8 or y2 - y1 < 20:
            return 0.0

        gray = cv2.cvtColor(image[y1:y2, x1:x2], cv2.COLOR_BGR2GRAY)
        edges = cv2.Canny(gray, 20, 60)
        lines = cv2.HoughLinesP(
            edges,
            1,
            np.pi / 180.0,
            threshold=12,
            minLineLength=15,
            maxLineGap=8,
        )
        longest = 0
        if lines is not None:
            for line in lines[:, 0]:
                dx = abs(int(line[2]) - int(line[0]))
                dy = abs(int(line[3]) - int(line[1]))
                if dy >= 15 and dx <= max(3, int(round(dy * 0.25))):
                    longest = max(longest, dy)
        return longest / float(max(1, gray.shape[0]))

    def evaluate(
        self,
        stream: str,
        file_index: int,
        pano_x: int,
        pano_y: int,
        pano_width: int,
        image: np.ndarray,
        detection: Detection,
        wall_time: float | None = None,
    ) -> StaticClutterDecision:
        if not self.enabled:
            return StaticClutterDecision()

        now = time.monotonic() if wall_time is None else wall_time
        self.tracks = [track for track in self.tracks if now - track.last_wall_time <= 180.0]
        appearance_hash = self._appearance_hash(image, detection)
        context_score = self._vertical_support_score(image, detection)
        box_width = max(1.0, detection.x2 - detection.x1)
        box_height = max(1.0, detection.y2 - detection.y1)
        box_area = box_width * box_height
        box_aspect = box_width / box_height

        best: StaticClutterTrack | None = None
        best_hash_distance = -1
        best_metric = float("inf")
        for track in self.tracks:
            if track.stream != stream or track.class_id != detection.class_id:
                continue
            dx = self._wrapped_distance(pano_x, track.anchor_x, pano_width)
            dy = abs(pano_y - track.anchor_y)
            if dx > 24 or dy > 12:
                continue
            hash_distance = (appearance_hash ^ track.reference_hash).bit_count()
            if hash_distance > 12:
                continue
            area_ratio = self._positive_ratio(box_area, track.reference_area)
            aspect_ratio = self._positive_ratio(box_aspect, track.reference_aspect)
            area_limit = 2.2 if track.suppressed else 1.8
            if area_ratio > area_limit or aspect_ratio > 1.8:
                continue
            metric = dx + dy * 2.0 + hash_distance * 1.5
            if track.suppressed:
                metric -= 20.0
            if metric < best_metric:
                best = track
                best_hash_distance = hash_distance
                best_metric = metric

        if best is None:
            self.next_track_id += 1
            track = StaticClutterTrack(
                track_id=self.next_track_id,
                stream=stream,
                class_id=detection.class_id,
                pano_width=pano_width,
                anchor_x=pano_x,
                anchor_y=pano_y,
                reference_hash=appearance_hash,
                reference_area=box_area,
                reference_aspect=box_aspect,
                first_wall_time=now,
                last_wall_time=now,
                last_file_index=file_index,
                context_hits=1 if context_score >= self.context_threshold else 0,
            )
            self.tracks.append(track)
            return StaticClutterDecision(
                track_id=track.track_id,
                stable_hits=track.stable_hits,
                context_hits=track.context_hits,
                context_score=context_score,
            )

        if file_index != best.last_file_index:
            best.stable_hits += 1
            if context_score >= self.context_threshold:
                best.context_hits += 1
            best.last_file_index = file_index
        best.last_wall_time = now
        duration_ms = (now - best.first_wall_time) * 1000.0
        was_suppressed = best.suppressed
        best.suppressed = best.suppressed or (
            best.stable_hits >= self.min_hits
            and best.context_hits >= self.min_context_hits
            and duration_ms >= self.min_duration_ms
        )
        return StaticClutterDecision(
            track_id=best.track_id,
            stable_hits=best.stable_hits,
            context_hits=best.context_hits,
            context_score=context_score,
            hash_distance=best_hash_distance,
            suppressed=best.suppressed,
            newly_suppressed=best.suppressed and not was_suppressed,
        )


class DirectYoloWorker:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        cv2.setNumThreads(4)
        ort.preload_dlls()

        session_options = ort.SessionOptions()
        session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        providers: list[Any] = [
            (
                "CUDAExecutionProvider",
                {
                    "device_id": 0,
                    "cudnn_conv_algo_search": "HEURISTIC",
                    "do_copy_in_default_stream": 1,
                },
            ),
            "CPUExecutionProvider",
        ]
        self.session = ort.InferenceSession(
            str(args.model),
            sess_options=session_options,
            providers=providers,
        )
        self.input = self.session.get_inputs()[0]
        self.output = self.session.get_outputs()[0]
        self.input_name = self.input.name
        self.input_size = args.input_size
        metadata = self.session.get_modelmeta().custom_metadata_map
        configured_names = [
            item.strip() for item in args.class_names.split(",") if item.strip()
        ]
        metadata_names: list[str] = []
        try:
            parsed_names = ast.literal_eval(metadata.get("names", "{}"))
            if isinstance(parsed_names, dict):
                metadata_names = [
                    str(parsed_names[index]) for index in sorted(parsed_names)
                ]
        except (SyntaxError, ValueError, KeyError, TypeError):
            metadata_names = []
        self.class_names = configured_names or metadata_names
        if not self.class_names:
            raise RuntimeError("model class names are unavailable")
        self.end_to_end = metadata.get("end2end", "").lower() == "true"

        input_shape = self.input.shape
        if len(input_shape) != 4:
            raise RuntimeError(f"unsupported model input shape: {input_shape}")
        fixed_height = input_shape[2] if isinstance(input_shape[2], int) else 0
        fixed_width = input_shape[3] if isinstance(input_shape[3], int) else 0
        if (fixed_height and fixed_height != self.input_size) or (
            fixed_width and fixed_width != self.input_size
        ):
            raise RuntimeError(
                f"configured input {self.input_size} does not match model "
                f"{fixed_width}x{fixed_height}"
            )
        self.infer_ewma_ms = 5.0
        self.request_count = 0
        self.mask_cache: dict[tuple[str, int, int, int, int], tuple[np.ndarray, list[tuple[int, int]]]] = {}
        self.window_cursor: dict[tuple[str, int], int] = {}
        self.history: deque[HistoryDetection] = deque()
        self.static_clutter = StaticClutterVerifier(args)

        warmup = np.zeros(
            (1, 3, self.input_size, self.input_size),
            dtype=np.float32,
        )
        started = time.perf_counter()
        for _ in range(5):
            self.session.run([self.output.name], {self.input_name: warmup})
        warmup_ms = (time.perf_counter() - started) * 1000.0
        emit_json(
            {
                "type": "ready",
                "model": str(args.model),
                "provider": self.session.get_providers()[0],
                "inputShape": "x".join(str(value) for value in self.input.shape),
                "classes": self.class_names,
                "decoder": "end_to_end" if self.end_to_end else "raw",
                "staticClutter": self.static_clutter.enabled,
                "warmupMs": warmup_ms,
            }
        )

    def infer(
        self,
        image: np.ndarray,
        branch: str,
        offset_x: int = 0,
        offset_y: int = 0,
        output_scale_x: float = 1.0,
        output_scale_y: float = 1.0,
    ) -> tuple[list[Detection], float]:
        resized = cv2.resize(
            image,
            (self.input_size, self.input_size),
            interpolation=cv2.INTER_LINEAR,
        )
        rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
        blob = np.ascontiguousarray(
            rgb.transpose(2, 0, 1)[None],
            dtype=np.float32,
        )
        blob *= 1.0 / 255.0

        started = time.perf_counter()
        output = self.session.run([self.output.name], {self.input_name: blob})[0]
        infer_ms = (time.perf_counter() - started) * 1000.0
        self.infer_ewma_ms = self.infer_ewma_ms * 0.90 + infer_ms * 0.10

        rows = np.asarray(output, dtype=np.float32)
        if rows.ndim == 3:
            rows = rows[0]
        if rows.ndim != 2:
            return [], infer_ms
        if rows.shape[0] <= 256 and rows.shape[1] > rows.shape[0]:
            rows = rows.T
        if rows.shape[1] < 6:
            return [], infer_ms

        parse_floor = max(0.01, self.args.low_threshold * 0.5)
        boxes = rows[:, :4]
        if self.end_to_end:
            confidences = rows[:, 4]
            class_ids = np.rint(rows[:, 5]).astype(np.int32)
            keep = (
                (confidences >= parse_floor)
                & (class_ids >= 0)
                & (class_ids < len(self.class_names))
                & (boxes[:, 2] > boxes[:, 0])
                & (boxes[:, 3] > boxes[:, 1])
            )
        else:
            class_count = min(len(self.class_names), rows.shape[1] - 4)
            if class_count <= 0:
                return [], infer_ms
            class_scores = rows[:, 4 : 4 + class_count]
            class_ids = np.argmax(class_scores, axis=1).astype(np.int32)
            confidences = class_scores[np.arange(class_scores.shape[0]), class_ids]
            keep = (
                (confidences >= parse_floor)
                & (boxes[:, 2] > 0.0)
                & (boxes[:, 3] > 0.0)
            )
        if not np.any(keep):
            return [], infer_ms

        boxes = boxes[keep]
        confidences = confidences[keep]
        class_ids = class_ids[keep]
        scale_to_source_x = image.shape[1] / float(self.input_size)
        scale_to_source_y = image.shape[0] / float(self.input_size)

        detections: list[Detection] = []
        for box, confidence, class_id_value in zip(boxes, confidences, class_ids):
            class_id = int(class_id_value)
            if self.end_to_end:
                x1, y1, x2, y2 = (float(value) for value in box)
                x1 *= scale_to_source_x
                y1 *= scale_to_source_y
                x2 *= scale_to_source_x
                y2 *= scale_to_source_y
            else:
                cx, cy, width, height = (float(value) for value in box)
                x1 = (cx - width * 0.5) * scale_to_source_x
                y1 = (cy - height * 0.5) * scale_to_source_y
                x2 = (cx + width * 0.5) * scale_to_source_x
                y2 = (cy + height * 0.5) * scale_to_source_y
            detections.append(
                Detection(
                    x1=offset_x + x1 * output_scale_x,
                    y1=offset_y + y1 * output_scale_y,
                    x2=offset_x + x2 * output_scale_x,
                    y2=offset_y + y2 * output_scale_y,
                    confidence=float(confidence),
                    class_id=class_id,
                    class_name=self.class_names[class_id],
                    branch=branch,
                )
            )
        return detections, infer_ms

    def sky_windows(
        self,
        request: dict[str, Any],
        frame_width: int,
        frame_height: int,
    ) -> tuple[np.ndarray | None, list[tuple[int, int]]]:
        mask_path = str(request.get("skyMaskPath", ""))
        tile_index = int(request.get("tileIndex", 0))
        segments = max(1, int(request.get("segments", 1)))
        shrink = max(0, int(request.get("skyShrinkPixels", 0)))
        if not mask_path or not Path(mask_path).is_file():
            return None, []

        key = (mask_path, tile_index, frame_width, frame_height, shrink)
        cached = self.mask_cache.get(key)
        if cached is not None:
            return cached

        panorama_mask = cv2.imread(mask_path, cv2.IMREAD_GRAYSCALE)
        if panorama_mask is None or panorama_mask.size == 0:
            return None, []
        small_slice_width = panorama_mask.shape[1] // segments
        x0 = tile_index * small_slice_width
        x1 = x0 + small_slice_width
        if small_slice_width <= 0 or x0 < 0 or x1 > panorama_mask.shape[1]:
            return None, []
        mask = panorama_mask[:, x0:x1].copy()
        if shrink > 0:
            scale = frame_width / float(max(1, mask.shape[1]))
            small_shrink = max(1, int(round(shrink / max(1.0, scale))))
            kernel = cv2.getStructuringElement(
                cv2.MORPH_ELLIPSE,
                (small_shrink * 2 + 1, small_shrink * 2 + 1),
            )
            mask = cv2.erode(mask, kernel)

        integral = cv2.integral((mask > 0).astype(np.uint8), sdepth=cv2.CV_32S)
        scale_x = mask.shape[1] / float(frame_width)
        scale_y = mask.shape[0] / float(frame_height)
        window = self.args.window_size
        windows: list[tuple[int, int]] = []
        for y in axis_starts(frame_height, window, self.args.window_stride):
            for x in axis_starts(frame_width, window, self.args.window_stride):
                mx1 = max(0, min(mask.shape[1] - 1, int(math.floor(x * scale_x))))
                my1 = max(0, min(mask.shape[0] - 1, int(math.floor(y * scale_y))))
                mx2 = max(mx1 + 1, min(mask.shape[1], int(math.ceil((x + window) * scale_x))))
                my2 = max(my1 + 1, min(mask.shape[0], int(math.ceil((y + window) * scale_y))))
                pixels = (
                    int(integral[my2, mx2])
                    - int(integral[my1, mx2])
                    - int(integral[my2, mx1])
                    + int(integral[my1, mx1])
                )
                coverage = pixels / float(max(1, (mx2 - mx1) * (my2 - my1)))
                if coverage >= self.args.sky_coverage:
                    windows.append((x, y))
        self.mask_cache[key] = (mask, windows)
        return mask, windows

    def is_sky_center(
        self,
        mask: np.ndarray | None,
        detection: Detection,
        frame_width: int,
        frame_height: int,
    ) -> bool:
        if mask is None:
            return True
        x = max(0, min(mask.shape[1] - 1, int(detection.cx * mask.shape[1] / frame_width)))
        y = max(0, min(mask.shape[0] - 1, int(detection.cy * mask.shape[0] / frame_height)))
        return bool(mask[y, x])

    def pano_position(
        self,
        request: dict[str, Any],
        detection: Detection,
        frame_width: int,
        frame_height: int,
    ) -> tuple[int, int]:
        pano_width = max(1, int(request.get("panoW", frame_width)))
        pano_height = max(1, int(request.get("panoH", frame_height)))
        slice_width = max(1, int(request.get("sliceW", frame_width)))
        tile_index = int(request.get("tileIndex", 0))
        pano_x = int(round(tile_index * slice_width + detection.cx * slice_width / frame_width))
        pano_y = int(round(detection.cy * pano_height / frame_height))
        return pano_x % pano_width, max(0, min(pano_height - 1, pano_y))

    def find_cross_stream_match(
        self,
        stream: str,
        class_id: int,
        pano_x: int,
        pano_y: int,
        pano_width: int,
        rx_ms: int,
    ) -> HistoryDetection | None:
        now = time.monotonic()
        while self.history and now - self.history[0].wall_time > 8.0:
            self.history.popleft()

        best: HistoryDetection | None = None
        best_metric = float("inf")
        for old in self.history:
            if old.stream == stream or old.class_id != class_id:
                continue
            if rx_ms > 0 and old.rx_ms > 0:
                delta_ms = abs(rx_ms - old.rx_ms)
                if delta_ms < 2500 or delta_ms > 5500:
                    continue
            dx = abs(pano_x - old.pano_x)
            dx = min(dx, pano_width - dx)
            dy = abs(pano_y - old.pano_y)
            if dx > 384 or dy > 256:
                continue
            metric = dx + dy * 1.5
            if metric < best_metric:
                best = old
                best_metric = metric
        return best

    def save_candidate(
        self,
        request: dict[str, Any],
        image: np.ndarray,
        detection: Detection,
        confidence: float,
        fused: bool,
        rank: int,
        static_decision: StaticClutterDecision,
    ) -> dict[str, Any] | None:
        stream = str(request.get("stream", "")).upper()
        source_path = Path(str(request.get("sourcePath", "")))
        request_id = str(request.get("requestId", "0"))
        file_index = str(request.get("fileIdx", "0"))
        rx_ms = int(request.get("rxMs", 0))
        pano_width = max(1, int(request.get("panoW", image.shape[1])))
        pano_height = max(1, int(request.get("panoH", image.shape[0])))
        pano_x, pano_y = self.pano_position(
            request,
            detection,
            image.shape[1],
            image.shape[0],
        )
        angle = pano_x / float(pano_width) * 360.0

        center_x = int(round(detection.cx))
        center_y = int(round(detection.cy))
        crop, origin_x, origin_y = fixed_crop(
            image,
            center_x,
            center_y,
            self.args.crop_size,
        )
        roi_x1 = max(0, min(self.args.crop_size - 1, int(round(detection.x1 - origin_x))))
        roi_y1 = max(0, min(self.args.crop_size - 1, int(round(detection.y1 - origin_y))))
        roi_x2 = max(0, min(self.args.crop_size - 1, int(round(detection.x2 - origin_x))))
        roi_y2 = max(0, min(self.args.crop_size - 1, int(round(detection.y2 - origin_y))))
        if roi_x2 <= roi_x1 or roi_y2 <= roi_y1:
            return None

        match = FRAME_TIME_RE.search(source_path.name)
        if match:
            date_dir = match.group(1)
            hour_dir = match.group(2)[:2]
        else:
            frame_time = time.localtime(rx_ms / 1000.0 if rx_ms > 0 else time.time())
            date_dir = time.strftime("%Y%m%d", frame_time)
            hour_dir = time.strftime("%H", frame_time)
        output_dir = self.args.save_root / date_dir / hour_dir
        output_dir.mkdir(parents=True, exist_ok=True)
        output_name = (
            f"{source_path.stem}_direct_{request_id}_{rank}"
            f"_px{pano_x:05d}_py{pano_y:05d}.jpg"
        )
        crop_path = output_dir / output_name
        if not cv2.imwrite(
            str(crop_path),
            crop,
            [cv2.IMWRITE_JPEG_QUALITY, 95],
        ):
            return None

        slice_width = max(1, int(request.get("sliceW", image.shape[1])))
        tile_index = int(request.get("tileIndex", 0))
        pano_box_x1 = int(round(tile_index * slice_width + detection.x1 * slice_width / image.shape[1])) % pano_width
        pano_box_x2 = int(round(tile_index * slice_width + detection.x2 * slice_width / image.shape[1])) % pano_width
        pano_box_y1 = max(0, min(pano_height - 1, int(round(detection.y1 * pano_height / image.shape[0]))))
        pano_box_y2 = max(0, min(pano_height - 1, int(round(detection.y2 * pano_height / image.shape[0]))))
        detector = f"direct_yolo_{detection.branch}"
        if fused:
            detector += "_rgb_bw_fused"

        manifest = {
            "file": crop_path.name,
            "path": str(crop_path),
            "source": source_path.name,
            "stream": stream,
            "date": date_dir,
            "hour": hour_dir,
            "angle": angle,
            "panoX": pano_x,
            "panoY": pano_y,
            "panoBoxX1": pano_box_x1,
            "panoBoxY1": pano_box_y1,
            "panoBoxX2": pano_box_x2,
            "panoBoxY2": pano_box_y2,
            "frameX": center_x,
            "frameY": center_y,
            "frameBoxX1": int(round(detection.x1)),
            "frameBoxY1": int(round(detection.y1)),
            "frameBoxX2": int(round(detection.x2)),
            "frameBoxY2": int(round(detection.y2)),
            "roiBoxX1": roi_x1,
            "roiBoxY1": roi_y1,
            "roiBoxX2": roi_x2,
            "roiBoxY2": roi_y2,
            "tileIndex": tile_index,
            "detector": detector,
            "classId": detection.class_id,
            "className": detection.class_name,
            "yoloScore": confidence,
            "yoloRawScore": detection.confidence,
            "crossStreamFused": fused,
            "score": confidence * 255.0,
            "fileIdx": file_index,
            "rxMs": str(rx_ms),
            "skyMaskMode": "temporal_sky_v1_direct",
            "staticTrackId": static_decision.track_id,
            "staticStableHits": static_decision.stable_hits,
            "staticContextHits": static_decision.context_hits,
            "staticContextScore": static_decision.context_score,
            "staticHashDistance": static_decision.hash_distance,
            "staticSuppressed": static_decision.suppressed,
        }
        with (output_dir / "direct_yolo_manifest.jsonl").open(
            "a",
            encoding="utf-8",
        ) as handle:
            handle.write(json.dumps(manifest, ensure_ascii=True, separators=(",", ":")) + "\n")

        return {
            "stream": stream,
            "angle": angle,
            "panoX": pano_x,
            "panoY": pano_y,
            "score": confidence * 255.0,
            "cropPath": str(crop_path),
            "roiBoxX1": roi_x1,
            "roiBoxY1": roi_y1,
            "roiBoxX2": roi_x2,
            "roiBoxY2": roi_y2,
            "detector": detector,
            "classId": detection.class_id,
            "className": detection.class_name,
            "confidence": confidence,
            "staticTrackId": static_decision.track_id,
            "staticStableHits": static_decision.stable_hits,
            "staticContextHits": static_decision.context_hits,
            "staticContextScore": static_decision.context_score,
            "staticHashDistance": static_decision.hash_distance,
            "staticSuppressed": static_decision.suppressed,
            "staticNewlySuppressed": static_decision.newly_suppressed,
        }

    def process_frame(self, request: dict[str, Any]) -> dict[str, Any]:
        total_started = time.perf_counter()
        submit_ms = int(request.get("submitMs", 0))
        queue_delay_ms = max(0.0, time.time() * 1000.0 - submit_ms) if submit_ms > 0 else 0.0
        budget_ms = (
            self.args.catchup_budget_ms
            if queue_delay_ms > self.args.max_queue_delay_ms
            else self.args.frame_budget_ms
        )
        stream = str(request.get("stream", "")).upper()
        source_path = Path(str(request.get("sourcePath", "")))

        decode_started = time.perf_counter()
        image = cv2.imread(str(source_path), cv2.IMREAD_COLOR)
        if image is None:
            raise RuntimeError(f"failed to decode frame: {source_path}")
        image = rotate_for_panorama(image)
        decode_ms = (time.perf_counter() - decode_started) * 1000.0
        height, width = image.shape[:2]

        quality_image = cv2.cvtColor(
            cv2.resize(
                image,
                (256, 256),
                interpolation=cv2.INTER_AREA,
            ),
            cv2.COLOR_BGR2GRAY,
        )
        contrast = float(np.std(quality_image))
        sharpness = float(cv2.Laplacian(quality_image, cv2.CV_32F).var())

        mask, windows = self.sky_windows(request, width, height)
        all_detections: list[Detection] = []
        infer_ms = 0.0
        global_detections, elapsed = self.infer(image, "global")
        all_detections.extend(
            detection
            for detection in global_detections
            if self.is_sky_center(mask, detection, width, height)
        )
        infer_ms += elapsed

        local_runs = 0
        cursor_key = (stream, int(request.get("tileIndex", 0)))
        cursor = self.window_cursor.get(cursor_key, 0)
        if windows:
            cursor %= len(windows)
            ordered_windows = windows[cursor:] + windows[:cursor]
        else:
            ordered_windows = []

        for x, y in ordered_windows:
            elapsed_total_ms = (time.perf_counter() - total_started) * 1000.0
            expected_next_ms = max(5.0, self.infer_ewma_ms * 1.35)
            if local_runs >= 2 and elapsed_total_ms + expected_next_ms > budget_ms:
                break
            window = image[
                y : y + self.args.window_size,
                x : x + self.args.window_size,
            ]
            detections, elapsed = self.infer(
                window,
                "local",
                offset_x=x,
                offset_y=y,
            )
            infer_ms += elapsed
            local_runs += 1
            for detection in detections:
                if self.is_sky_center(mask, detection, width, height):
                    all_detections.append(detection)

        if windows:
            self.window_cursor[cursor_key] = (cursor + local_runs) % len(windows)

        all_detections = non_max_suppression(
            all_detections,
            self.args.iou_threshold,
        )
        pano_width = max(1, int(request.get("panoW", width)))
        rx_ms = int(request.get("rxMs", 0))
        output_pool: list[tuple[float, Detection, bool]] = []
        history_additions: list[HistoryDetection] = []
        for detection in all_detections:
            if detection.confidence < self.args.low_threshold:
                continue
            pano_x, pano_y = self.pano_position(request, detection, width, height)
            counterpart = self.find_cross_stream_match(
                stream,
                detection.class_id,
                pano_x,
                pano_y,
                pano_width,
                rx_ms,
            )
            fused = counterpart is not None
            confidence = detection.confidence
            if counterpart is not None:
                confidence = 1.0 - (1.0 - confidence) * (1.0 - counterpart.confidence)
            if detection.confidence >= self.args.high_threshold or (
                fused and confidence >= self.args.high_threshold
            ):
                output_pool.append((confidence, detection, fused))
            history_additions.append(
                HistoryDetection(
                    stream=stream,
                    class_id=detection.class_id,
                    pano_x=pano_x,
                    pano_y=pano_y,
                    confidence=detection.confidence,
                    rx_ms=rx_ms,
                    wall_time=time.monotonic(),
                )
            )

        for history_detection in history_additions[:12]:
            self.history.append(history_detection)

        output_pool.sort(key=lambda item: item[0], reverse=True)
        candidates: list[dict[str, Any]] = []
        for rank, (confidence, detection, fused) in enumerate(
            output_pool[: self.args.max_output],
            1,
        ):
            pano_x, pano_y = self.pano_position(request, detection, width, height)
            static_decision = self.static_clutter.evaluate(
                stream=stream,
                file_index=int(request.get("fileIdx", 0)),
                pano_x=pano_x,
                pano_y=pano_y,
                pano_width=pano_width,
                image=image,
                detection=detection,
            )
            saved = self.save_candidate(
                request,
                image,
                detection,
                confidence,
                fused,
                rank,
                static_decision,
            )
            if saved is not None:
                candidates.append(saved)

        total_ms = (time.perf_counter() - total_started) * 1000.0
        self.request_count += 1
        return {
            "type": "result",
            "requestId": str(request.get("requestId", "0")),
            "stream": stream,
            "fileIdx": str(request.get("fileIdx", "0")),
            "globalRuns": 1,
            "localRuns": local_runs,
            "localWindows": len(windows),
            "candidates": candidates,
            "queueDelayMs": queue_delay_ms,
            "decodeMs": decode_ms,
            "inferMs": infer_ms,
            "totalMs": total_ms,
            "budgetMs": budget_ms,
            "contrast": contrast,
            "sharpness": sharpness,
            "log": self.request_count % 16 == 0,
        }


def main() -> int:
    args = parse_args()
    try:
        worker = DirectYoloWorker(args)
    except Exception as exc:
        emit_json({"type": "error", "message": f"worker initialization failed: {exc}"})
        return 2

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        request: dict[str, Any] = {}
        try:
            request = json.loads(line)
            if request.get("type") != "frame":
                continue
            emit_json(worker.process_frame(request))
        except Exception as exc:
            emit_json(
                {
                    "type": "result",
                    "requestId": str(request.get("requestId", "0")),
                    "stream": str(request.get("stream", "")),
                    "fileIdx": str(request.get("fileIdx", "0")),
                    "globalRuns": 0,
                    "localRuns": 0,
                    "localWindows": 0,
                    "candidates": [],
                    "queueDelayMs": 0.0,
                    "decodeMs": 0.0,
                    "inferMs": 0.0,
                    "totalMs": 0.0,
                    "budgetMs": args.frame_budget_ms,
                    "log": True,
                    "error": str(exc),
                }
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
