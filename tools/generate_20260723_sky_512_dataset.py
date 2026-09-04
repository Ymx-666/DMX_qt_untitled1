#!/usr/bin/env python3
"""Export every sky-intersecting 512x512 window from the 20260723 raw frames.

The raw device frames are oriented exactly as in ``videothread.cpp``:

1. rotate 90 degrees counter-clockwise;
2. mirror the rotated image horizontally.

The accepted 20260723 panorama sky mask is stored at quarter scale.  A raw
frame index identifies one of 16 panorama slots.  RGB uses the synthetic
left-turn slot directly, while BW adds the same 180-degree alignment offset
used by the DMX application.

The mask selects windows only.  Saved images keep their original RGB/BW
pixels, including non-sky context in a boundary window.  A window is kept when
it contains at least one sky-mask pixel by default.  Any incomplete window at
the right or bottom image edge is padded with black to the requested size.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Mapping, Sequence, Set, Tuple

import cv2
import numpy as np


DEFAULT_SOURCE_ROOT = Path("/mnt/dmx_share/raw/20260723")
DEFAULT_OUT_DIR = Path("/mnt/dmx4t/DMX_yangben/20260723")
DEFAULT_MASK = Path(
    "/mnt/dmx4t/data/dmx_test/analysis/"
    "sky_mask_20260723_geometry_v1_shrink64_20260730/"
    "bw_04_mask_after_shrink_64px.png"
)
DEFAULT_MASK_MANIFEST = DEFAULT_MASK.parent / "manifest.json"
FRAME_RE = re.compile(
    r"^(?P<stream>RGB|BW)_(?P<day>\d{8})_(?P<time>\d{6})_(?P<index>\d+)\.jpg$",
    re.IGNORECASE,
)

CROP_FIELDS = [
    "file",
    "source",
    "stream",
    "time_group",
    "source_index",
    "source_slot",
    "world_tile",
    "mask_file",
    "x",
    "y",
    "width",
    "height",
    "crop_size",
    "valid_width",
    "valid_height",
    "pad_right",
    "pad_bottom",
    "sky_pixels",
    "sky_ratio",
]

SOURCE_FIELDS = [
    "source",
    "stream",
    "time_group",
    "source_index",
    "source_slot",
    "world_tile",
    "image_width",
    "image_height",
    "mask_file",
    "mask_sky_pixels",
    "mask_sky_ratio",
    "selected_windows",
    "saved_windows",
]


@dataclass(frozen=True)
class SourceImage:
    path: Path
    stream: str
    time_group: str
    index: int

    @property
    def slot(self) -> int:
        return self.index % 16

    @property
    def world_tile(self) -> int:
        left_turn_tile = (16 - self.slot) % 16
        if self.stream == "BW":
            return (left_turn_tile + 8) % 16
        return left_turn_tile


@dataclass(frozen=True)
class WindowPlan:
    x: int
    y: int
    valid_width: int
    valid_height: int
    sky_pixels: int
    sky_ratio: float


@dataclass
class ProcessResult:
    source: SourceImage
    image_width: int
    image_height: int
    mask_file: str
    mask_sky_pixels: int
    mask_sky_ratio: float
    selected_windows: int
    saved_windows: int
    rows: List[Dict[str, object]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--mask", type=Path, default=DEFAULT_MASK)
    parser.add_argument(
        "--streams",
        nargs="+",
        choices=("RGB", "BW"),
        default=["RGB", "BW"],
        help="Streams to export.",
    )
    parser.add_argument("--crop-size", type=int, default=512)
    parser.add_argument(
        "--min-sky-ratio",
        type=float,
        default=0.0,
        help=(
            "Minimum sky coverage for a window. The default 0 keeps every "
            "window containing at least one sky pixel."
        ),
    )
    parser.add_argument("--segments", type=int, default=16)
    parser.add_argument("--full-frame-width", type=int, default=4096)
    parser.add_argument("--full-frame-height", type=int, default=4096)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--jpeg-quality", type=int, default=95)
    parser.add_argument(
        "--limit-sources",
        type=int,
        default=0,
        help="Process at most this many sources per stream; 0 means all.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Rewrite crop files that already exist.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate inputs and report the planned counts without decoding frames.",
    )
    parser.add_argument("--progress-every", type=int, default=100)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.crop_size <= 0:
        raise ValueError("--crop-size must be positive")
    if not 0.0 <= args.min_sky_ratio <= 1.0:
        raise ValueError("--min-sky-ratio must be in [0, 1]")
    if args.segments <= 0:
        raise ValueError("--segments must be positive")
    if args.full_frame_width <= 0 or args.full_frame_height <= 0:
        raise ValueError("full frame dimensions must be positive")
    if args.workers <= 0:
        raise ValueError("--workers must be positive")
    if not 1 <= args.jpeg_quality <= 100:
        raise ValueError("--jpeg-quality must be in [1, 100]")
    if args.limit_sources < 0:
        raise ValueError("--limit-sources cannot be negative")


def discover_sources(
    source_root: Path,
    streams: Sequence[str],
    limit_per_stream: int,
) -> List[SourceImage]:
    sources: List[SourceImage] = []
    for stream in streams:
        stream_root = source_root / stream
        if not stream_root.is_dir():
            raise FileNotFoundError(f"source stream directory not found: {stream_root}")
        stream_sources: List[SourceImage] = []
        for path in stream_root.rglob("*.jpg"):
            match = FRAME_RE.fullmatch(path.name)
            if not match or match.group("stream").upper() != stream:
                continue
            stream_sources.append(
                SourceImage(
                    path=path,
                    stream=stream,
                    time_group=path.parent.name,
                    index=int(match.group("index")),
                )
            )
        stream_sources.sort(key=lambda item: (item.index, str(item.path)))
        if not stream_sources:
            raise RuntimeError(f"no valid {stream} JPEG images below {stream_root}")
        seen: Set[int] = set()
        duplicate_indices: List[int] = []
        for source in stream_sources:
            if source.index in seen:
                duplicate_indices.append(source.index)
            seen.add(source.index)
        if duplicate_indices:
            raise RuntimeError(
                f"{stream} contains duplicate frame indices: {duplicate_indices[:10]}"
            )
        if limit_per_stream > 0:
            stream_sources = stream_sources[:limit_per_stream]
        sources.extend(stream_sources)
    return sources


def load_mask_metadata(mask_path: Path) -> Mapping[str, object]:
    manifest_path = mask_path.parent / "manifest.json"
    if manifest_path.is_file():
        with manifest_path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    return {}


def load_mask_atlas(mask_path: Path, segments: int) -> np.ndarray:
    mask = cv2.imread(str(mask_path), cv2.IMREAD_GRAYSCALE)
    if mask is None:
        raise RuntimeError(f"failed to read sky mask: {mask_path}")
    mask = np.where(mask > 0, 255, 0).astype(np.uint8)
    if mask.shape[1] % segments != 0:
        raise RuntimeError(
            f"mask width {mask.shape[1]} is not divisible by {segments} segments"
        )
    return mask


def window_plan_for_mask(
    full_mask: np.ndarray,
    crop_size: int,
    min_sky_ratio: float,
) -> List[WindowPlan]:
    height, width = full_mask.shape
    plans: List[WindowPlan] = []
    for y in range(0, height, crop_size):
        valid_height = min(crop_size, height - y)
        for x in range(0, width, crop_size):
            valid_width = min(crop_size, width - x)
            roi = full_mask[y : y + valid_height, x : x + valid_width]
            sky_pixels = int(cv2.countNonZero(roi))
            if sky_pixels <= 0:
                continue
            # Black padding is not sky, so coverage uses the final crop area.
            sky_ratio = sky_pixels / float(crop_size * crop_size)
            if sky_ratio + 1e-12 < min_sky_ratio:
                continue
            plans.append(
                WindowPlan(
                    x=x,
                    y=y,
                    valid_width=valid_width,
                    valid_height=valid_height,
                    sky_pixels=sky_pixels,
                    sky_ratio=sky_ratio,
                )
            )
    return plans


def prepare_tile_masks(
    atlas: np.ndarray,
    out_dir: Path,
    segments: int,
    full_width: int,
    full_height: int,
    crop_size: int,
    min_sky_ratio: float,
) -> Tuple[Dict[int, List[WindowPlan]], Dict[int, Tuple[int, float, str]]]:
    small_slice_width = atlas.shape[1] // segments
    plans: Dict[int, List[WindowPlan]] = {}
    stats: Dict[int, Tuple[int, float, str]] = {}
    mask_dir = out_dir / "masks" / "world_tiles"
    mask_dir.mkdir(parents=True, exist_ok=True)
    for tile in range(segments):
        small = atlas[:, tile * small_slice_width : (tile + 1) * small_slice_width]
        full = cv2.resize(
            small,
            (full_width, full_height),
            interpolation=cv2.INTER_NEAREST,
        )
        mask_relative = Path("masks") / "world_tiles" / f"tile_{tile:02d}.png"
        mask_path = out_dir / mask_relative
        if not mask_path.is_file() and not cv2.imwrite(str(mask_path), full):
            raise RuntimeError(f"failed to write tile mask: {mask_path}")
        tile_sky_pixels = int(cv2.countNonZero(full))
        tile_sky_ratio = tile_sky_pixels / float(full.size)
        plans[tile] = window_plan_for_mask(full, crop_size, min_sky_ratio)
        stats[tile] = (tile_sky_pixels, tile_sky_ratio, str(mask_relative))
    return plans, stats


def write_stream_slot_map(out_dir: Path, segments: int) -> None:
    path = out_dir / "masks" / "stream_slot_map.csv"
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["stream", "source_slot", "world_tile", "mask_file"])
        for stream in ("RGB", "BW"):
            for slot in range(segments):
                left_turn_tile = (segments - slot) % segments
                world_tile = (
                    (left_turn_tile + segments // 2) % segments
                    if stream == "BW"
                    else left_turn_tile
                )
                writer.writerow(
                    [
                        stream,
                        slot,
                        world_tile,
                        f"masks/world_tiles/tile_{world_tile:02d}.png",
                    ]
                )


def orient_like_dmx(image: np.ndarray) -> np.ndarray:
    rotated = cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return cv2.flip(rotated, 1)


def crop_with_black(
    image: np.ndarray,
    plan: WindowPlan,
    crop_size: int,
) -> np.ndarray:
    crop = np.zeros((crop_size, crop_size, 3), dtype=np.uint8)
    roi = image[
        plan.y : plan.y + plan.valid_height,
        plan.x : plan.x + plan.valid_width,
    ]
    crop[: plan.valid_height, : plan.valid_width] = roi
    return crop


def atomic_write_jpg(path: Path, image: np.ndarray, quality: int) -> None:
    temp_path = path.with_name(f".{path.stem}.{os.getpid()}.tmp.jpg")
    ok = cv2.imwrite(
        str(temp_path),
        image,
        [int(cv2.IMWRITE_JPEG_QUALITY), int(quality)],
    )
    if not ok:
        raise RuntimeError(f"failed to write JPEG: {temp_path}")
    os.replace(temp_path, path)


def crop_relative_path(source: SourceImage, plan: WindowPlan) -> Path:
    return (
        Path("images")
        / source.stream
        / source.time_group
        / f"{source.path.stem}_x{plan.x:04d}_y{plan.y:04d}.jpg"
    )


def process_source(
    source: SourceImage,
    out_dir: Path,
    plans_by_tile: Mapping[int, Sequence[WindowPlan]],
    tile_stats: Mapping[int, Tuple[int, float, str]],
    full_width: int,
    full_height: int,
    crop_size: int,
    jpeg_quality: int,
    overwrite: bool,
) -> ProcessResult:
    plans = plans_by_tile[source.world_tile]
    mask_sky_pixels, mask_sky_ratio, mask_file = tile_stats[source.world_tile]
    if not plans:
        return ProcessResult(
            source=source,
            image_width=full_width,
            image_height=full_height,
            mask_file=mask_file,
            mask_sky_pixels=mask_sky_pixels,
            mask_sky_ratio=mask_sky_ratio,
            selected_windows=0,
            saved_windows=0,
            rows=[],
        )

    image = cv2.imread(str(source.path), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"failed to decode source image: {source.path}")
    image = orient_like_dmx(image)
    height, width = image.shape[:2]
    if width != full_width or height != full_height:
        raise RuntimeError(
            f"unexpected oriented size for {source.path}: "
            f"{width}x{height}, expected {full_width}x{full_height}"
        )

    rows: List[Dict[str, object]] = []
    saved = 0
    output_parent = out_dir / "images" / source.stream / source.time_group
    output_parent.mkdir(parents=True, exist_ok=True)
    for plan in plans:
        relative = crop_relative_path(source, plan)
        output_path = out_dir / relative
        if overwrite or not output_path.is_file():
            crop = crop_with_black(image, plan, crop_size)
            atomic_write_jpg(output_path, crop, jpeg_quality)
        saved += 1
        rows.append(
            {
                "file": str(relative),
                "source": str(source.path),
                "stream": source.stream,
                "time_group": source.time_group,
                "source_index": source.index,
                "source_slot": source.slot,
                "world_tile": source.world_tile,
                "mask_file": mask_file,
                "x": plan.x,
                "y": plan.y,
                "width": crop_size,
                "height": crop_size,
                "crop_size": crop_size,
                "valid_width": plan.valid_width,
                "valid_height": plan.valid_height,
                "pad_right": crop_size - plan.valid_width,
                "pad_bottom": crop_size - plan.valid_height,
                "sky_pixels": plan.sky_pixels,
                "sky_ratio": f"{plan.sky_ratio:.8f}",
            }
        )
    return ProcessResult(
        source=source,
        image_width=width,
        image_height=height,
        mask_file=mask_file,
        mask_sky_pixels=mask_sky_pixels,
        mask_sky_ratio=mask_sky_ratio,
        selected_windows=len(plans),
        saved_windows=saved,
        rows=rows,
    )


def read_resume_state(
    manifest_path: Path,
    source_summary_path: Path,
) -> Tuple[Set[str], Dict[str, int], Dict[str, int]]:
    existing_files: Set[str] = set()
    manifest_counts: Dict[str, int] = {}
    if manifest_path.is_file():
        with manifest_path.open("r", newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                file_value = row.get("file", "")
                source_value = row.get("source", "")
                if file_value:
                    existing_files.add(file_value)
                if source_value:
                    manifest_counts[source_value] = manifest_counts.get(source_value, 0) + 1

    completed_counts: Dict[str, int] = {}
    if source_summary_path.is_file():
        with source_summary_path.open("r", newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                source_value = row.get("source", "")
                try:
                    saved = int(row.get("saved_windows", ""))
                except ValueError:
                    continue
                if source_value:
                    completed_counts[source_value] = saved
    return existing_files, manifest_counts, completed_counts


def open_csv_append(path: Path, fields: Sequence[str]):
    existed = path.is_file() and path.stat().st_size > 0
    handle = path.open("a", newline="", encoding="utf-8")
    writer = csv.DictWriter(handle, fieldnames=fields)
    if not existed:
        writer.writeheader()
        handle.flush()
    return handle, writer


def iter_pending_sources(
    sources: Sequence[SourceImage],
    plans_by_tile: Mapping[int, Sequence[WindowPlan]],
    manifest_counts: Mapping[str, int],
    completed_counts: Mapping[str, int],
) -> Iterator[SourceImage]:
    for source in sources:
        source_key = str(source.path)
        expected = len(plans_by_tile[source.world_tile])
        if (
            completed_counts.get(source_key) == expected
            and manifest_counts.get(source_key, 0) >= expected
        ):
            continue
        yield source


def source_summary_row(result: ProcessResult) -> Dict[str, object]:
    source = result.source
    return {
        "source": str(source.path),
        "stream": source.stream,
        "time_group": source.time_group,
        "source_index": source.index,
        "source_slot": source.slot,
        "world_tile": source.world_tile,
        "image_width": result.image_width,
        "image_height": result.image_height,
        "mask_file": result.mask_file,
        "mask_sky_pixels": result.mask_sky_pixels,
        "mask_sky_ratio": f"{result.mask_sky_ratio:.8f}",
        "selected_windows": result.selected_windows,
        "saved_windows": result.saved_windows,
    }


def aggregate_csv_counts(
    manifest_path: Path,
    source_summary_path: Path,
) -> Tuple[Dict[str, int], Dict[str, int]]:
    crop_counts: Dict[str, int] = {}
    with manifest_path.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            stream = row["stream"]
            crop_counts[stream] = crop_counts.get(stream, 0) + 1
    source_counts: Dict[str, int] = {}
    with source_summary_path.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            stream = row["stream"]
            source_counts[stream] = source_counts.get(stream, 0) + 1
    return crop_counts, source_counts


def write_dataset_summary(
    args: argparse.Namespace,
    mask_metadata: Mapping[str, object],
    discovered_counts: Mapping[str, int],
    crop_counts: Mapping[str, int],
    source_counts: Mapping[str, int],
) -> None:
    summary = {
        "sourceRoot": str(args.source_root),
        "outputRoot": str(args.out_dir),
        "skyMask": str(args.mask),
        "skyMaskMetadata": dict(mask_metadata),
        "orientation": "rotate_ccw_90_then_mirror_horizontal",
        "segments": args.segments,
        "bwWorldAlignmentOffsetTiles": args.segments // 2,
        "cropSize": args.crop_size,
        "minSkyRatio": args.min_sky_ratio,
        "windowSelection": (
            "keep_any_sky_intersection"
            if args.min_sky_ratio == 0.0
            else "keep_minimum_sky_ratio"
        ),
        "nonSkyPixels": "preserved_as_source_context",
        "incompleteWindowPadding": "black",
        "jpegQuality": args.jpeg_quality,
        "discoveredSources": dict(discovered_counts),
        "completedSources": dict(source_counts),
        "exportedCrops": dict(crop_counts),
    }
    path = args.out_dir / "dataset_summary.json"
    temp_path = path.with_suffix(".json.tmp")
    with temp_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(temp_path, path)


def main() -> int:
    args = parse_args()
    validate_args(args)
    args.streams = [stream.upper() for stream in args.streams]

    sources = discover_sources(args.source_root, args.streams, args.limit_sources)
    discovered_counts: Dict[str, int] = {}
    for source in sources:
        discovered_counts[source.stream] = discovered_counts.get(source.stream, 0) + 1

    atlas = load_mask_atlas(args.mask, args.segments)
    mask_metadata = load_mask_metadata(args.mask)
    if mask_metadata:
        manifest_segments = int(mask_metadata.get("segments", args.segments))
        if manifest_segments != args.segments:
            raise RuntimeError(
                f"mask manifest segments={manifest_segments}, requested={args.segments}"
            )
        manifest_slice_width = int(
            mask_metadata.get("fullSliceWidth", args.full_frame_width)
        )
        manifest_height = int(mask_metadata.get("fullHeight", args.full_frame_height))
        if (
            manifest_slice_width != args.full_frame_width
            or manifest_height != args.full_frame_height
        ):
            raise RuntimeError(
                "mask manifest full frame size "
                f"{manifest_slice_width}x{manifest_height}, requested "
                f"{args.full_frame_width}x{args.full_frame_height}"
            )

    args.out_dir.mkdir(parents=True, exist_ok=True)
    plans_by_tile, tile_stats = prepare_tile_masks(
        atlas=atlas,
        out_dir=args.out_dir,
        segments=args.segments,
        full_width=args.full_frame_width,
        full_height=args.full_frame_height,
        crop_size=args.crop_size,
        min_sky_ratio=args.min_sky_ratio,
    )
    write_stream_slot_map(args.out_dir, args.segments)

    planned_counts: Dict[str, int] = {}
    for source in sources:
        planned_counts[source.stream] = (
            planned_counts.get(source.stream, 0)
            + len(plans_by_tile[source.world_tile])
        )
    print(
        "[plan] "
        + " ".join(
            f"{stream}: sources={discovered_counts.get(stream, 0)} "
            f"crops={planned_counts.get(stream, 0)}"
            for stream in args.streams
        ),
        flush=True,
    )
    for tile in range(args.segments):
        print(
            f"[mask tile {tile:02d}] "
            f"sky_ratio={tile_stats[tile][1]:.6f} "
            f"windows={len(plans_by_tile[tile])}",
            flush=True,
        )
    if args.dry_run:
        print("[done] dry run; no source frames decoded and no crops written", flush=True)
        return 0

    manifest_path = args.out_dir / "manifest.csv"
    source_summary_path = args.out_dir / "source_summary.csv"
    existing_files, manifest_counts, completed_counts = read_resume_state(
        manifest_path, source_summary_path
    )
    pending = list(
        iter_pending_sources(
            sources,
            plans_by_tile,
            manifest_counts,
            completed_counts,
        )
    )
    print(
        f"[resume] pending_sources={len(pending)} "
        f"already_completed={len(sources) - len(pending)}",
        flush=True,
    )

    manifest_handle, manifest_writer = open_csv_append(manifest_path, CROP_FIELDS)
    summary_handle, summary_writer = open_csv_append(
        source_summary_path, SOURCE_FIELDS
    )
    processed = 0
    newly_recorded_crops = 0

    def run_one(source: SourceImage) -> ProcessResult:
        return process_source(
            source=source,
            out_dir=args.out_dir,
            plans_by_tile=plans_by_tile,
            tile_stats=tile_stats,
            full_width=args.full_frame_width,
            full_height=args.full_frame_height,
            crop_size=args.crop_size,
            jpeg_quality=args.jpeg_quality,
            overwrite=args.overwrite,
        )

    try:
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            for result in executor.map(run_one, pending):
                rows_to_add = [
                    row for row in result.rows if str(row["file"]) not in existing_files
                ]
                if rows_to_add:
                    manifest_writer.writerows(rows_to_add)
                    manifest_handle.flush()
                    for row in rows_to_add:
                        existing_files.add(str(row["file"]))
                    newly_recorded_crops += len(rows_to_add)
                summary_writer.writerow(source_summary_row(result))
                summary_handle.flush()
                processed += 1
                if (
                    processed == 1
                    or processed == len(pending)
                    or (
                        args.progress_every > 0
                        and processed % args.progress_every == 0
                    )
                ):
                    print(
                        f"[progress] sources={processed}/{len(pending)} "
                        f"new_manifest_crops={newly_recorded_crops} "
                        f"last={result.source.stream}:{result.source.index}",
                        flush=True,
                    )
    finally:
        manifest_handle.close()
        summary_handle.close()

    crop_counts, source_counts = aggregate_csv_counts(
        manifest_path, source_summary_path
    )
    write_dataset_summary(
        args,
        mask_metadata,
        discovered_counts,
        crop_counts,
        source_counts,
    )
    print(
        "[done] "
        + " ".join(
            f"{stream}: sources={source_counts.get(stream, 0)} "
            f"crops={crop_counts.get(stream, 0)}"
            for stream in args.streams
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[stopped] interrupted; rerun the same command to resume", file=sys.stderr)
        raise SystemExit(130)
