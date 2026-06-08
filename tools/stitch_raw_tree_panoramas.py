#!/usr/bin/env python3
"""Batch-stitch RGB/BW raw folders under one date directory."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import IO, List, Optional, Sequence, Tuple

try:
    from stitch_raw_folder_panoramas import (
        FrameRecord,
        group_frames,
        list_image_files,
        parse_size,
        safe_label,
        stitch_group,
        write_preview,
    )
except ModuleNotFoundError:
    from .stitch_raw_folder_panoramas import (
        FrameRecord,
        group_frames,
        list_image_files,
        parse_size,
        safe_label,
        stitch_group,
        write_preview,
    )


DEFAULT_ROOT = "/home/sht/data/20260608"
DEFAULT_OUTPUT = "/home/sht/data/all"
DEFAULT_STREAMS = ("RGB", "BW")


@dataclass(frozen=True)
class RawFolderJob:
    stream: str
    date_label: str
    folder_label: str
    source_dir: Path


def folder_sort_key(path: Path) -> Tuple[int, int, str]:
    name = path.name
    if name.isdigit():
        return (0, int(name), name)
    return (1, 0, name)


def discover_raw_folders(root: Path, streams: Sequence[str] = DEFAULT_STREAMS) -> List[RawFolderJob]:
    root = Path(root).expanduser()
    date_label = safe_label(root.name, "date")
    jobs: List[RawFolderJob] = []
    for stream in streams:
        stream_name = stream.strip().upper()
        stream_dir = root / stream_name
        if not stream_dir.is_dir():
            continue
        for source_dir in sorted((p for p in stream_dir.iterdir() if p.is_dir()), key=folder_sort_key):
            jobs.append(
                RawFolderJob(
                    stream=stream_name,
                    date_label=date_label,
                    folder_label=safe_label(source_dir.name, "folder"),
                    source_dir=source_dir,
                )
            )
    return jobs


def render_progress_line(
    *,
    done: int,
    total: int,
    current: str,
    panorama_count: int,
    dropped_tail_frames: int,
    width: int = 24,
) -> str:
    total = max(total, 1)
    done = max(0, min(done, total))
    filled = int(width * done / total)
    bar = "#" * filled + "-" * (width - filled)
    return (
        f"[{bar}] {done}/{total} folders | {current} | "
        f"{panorama_count} panoramas | dropped {dropped_tail_frames}"
    )


def _write_progress(
    stream: IO[str],
    *,
    done: int,
    total: int,
    current: str,
    panorama_count: int,
    dropped_tail_frames: int,
) -> None:
    stream.write(
        "\r"
        + render_progress_line(
            done=done,
            total=total,
            current=current,
            panorama_count=panorama_count,
            dropped_tail_frames=dropped_tail_frames,
        )
    )
    stream.flush()


def stitch_job(
    job: RawFolderJob,
    out_dir: Path,
    *,
    frames_per_panorama: int = 16,
    expected_frame_size: Optional[Tuple[int, int]] = (4096, 4096),
    preview_width: int = 8192,
    preview_quality: int = 95,
    orient: bool = True,
    mirror: bool = True,
    reverse: bool = True,
) -> dict:
    frames = list_image_files(job.source_dir)
    groups = group_frames(frames, frames_per_panorama)
    dropped_tail_frames = len(frames) % frames_per_panorama
    prefix = job.stream.lower()
    folder_manifest = {
        "stream": job.stream,
        "date": job.date_label,
        "folder": job.folder_label,
        "source": str(job.source_dir),
        "inputFrames": len(frames),
        "panoramaCount": len(groups),
        "droppedTailFrames": dropped_tail_frames,
        "panoramas": [],
    }

    for group_idx, group_paths in enumerate(groups, 1):
        ordered_paths = list(reversed(group_paths)) if reverse else list(group_paths)
        records = [
            FrameRecord(job.stream, 0, idx + 1, path)
            for idx, path in enumerate(ordered_paths)
        ]
        stem = f"{prefix}_{job.date_label}_{job.folder_label}_pano_{group_idx:04d}"
        pano_name = f"{stem}.tiff"
        preview_name = f"{stem}_preview.jpg"
        pano_path = out_dir / pano_name
        preview_path = out_dir / preview_name
        pano_w, pano_h, uncompressed_bytes = stitch_group(
            records,
            pano_path,
            expected_frame_size=expected_frame_size,
            orient=orient,
            mirror=mirror,
        )
        write_preview(pano_path, preview_path, preview_width, preview_quality)
        folder_manifest["panoramas"].append(
            {
                "file": pano_name,
                "preview": preview_name,
                "panoW": pano_w,
                "panoH": pano_h,
                "frames": [str(p) for p in ordered_paths],
                "uncompressedBytes": uncompressed_bytes,
            }
        )

    return folder_manifest


def batch_stitch_tree(
    root: Path,
    out_dir: Path,
    *,
    frames_per_panorama: int = 16,
    expected_frame_size: Optional[Tuple[int, int]] = (4096, 4096),
    preview_width: int = 8192,
    preview_quality: int = 95,
    orient: bool = True,
    mirror: bool = True,
    reverse: bool = True,
    show_progress: bool = True,
    progress_stream: Optional[IO[str]] = None,
) -> dict:
    root = Path(root).expanduser()
    if not root.is_dir():
        raise SystemExit(f"Root directory not found: {root}")
    out_dir = Path(out_dir).expanduser()
    out_dir.mkdir(parents=True, exist_ok=True)
    progress_stream = progress_stream if progress_stream is not None else sys.stderr

    jobs = discover_raw_folders(root)
    manifest = {
        "sourceRoot": str(root),
        "outDir": str(out_dir),
        "date": safe_label(root.name, "date"),
        "framesPerPanorama": frames_per_panorama,
        "expectedFrameSize": "any" if expected_frame_size is None else f"{expected_frame_size[0]}x{expected_frame_size[1]}",
        "orientation": (
            "rotate_ccw_90_then_horizontal_mirror"
            if orient and mirror else
            "rotate_ccw_90_only"
            if orient else
            "none"
        ),
        "reverseOrder": reverse,
        "totalFolders": len(jobs),
        "totalInputFrames": 0,
        "totalPanoramas": 0,
        "totalDroppedTailFrames": 0,
        "folders": [],
    }

    for idx, job in enumerate(jobs, 1):
        folder_manifest = stitch_job(
            job,
            out_dir,
            frames_per_panorama=frames_per_panorama,
            expected_frame_size=expected_frame_size,
            preview_width=preview_width,
            preview_quality=preview_quality,
            orient=orient,
            mirror=mirror,
            reverse=reverse,
        )
        manifest["folders"].append(folder_manifest)
        manifest["totalInputFrames"] += folder_manifest["inputFrames"]
        manifest["totalPanoramas"] += folder_manifest["panoramaCount"]
        manifest["totalDroppedTailFrames"] += folder_manifest["droppedTailFrames"]
        if show_progress:
            _write_progress(
                progress_stream,
                done=idx,
                total=len(jobs),
                current=f"{job.stream}/{job.folder_label}",
                panorama_count=folder_manifest["panoramaCount"],
                dropped_tail_frames=folder_manifest["droppedTailFrames"],
            )

    if show_progress:
        progress_stream.write("\n")
        progress_stream.flush()

    manifest_path = out_dir / "batch_stitch_manifest.json"
    with manifest_path.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return manifest


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Batch-stitch RGB/BW raw folders under one date directory.")
    p.add_argument("--root", type=Path, default=Path(DEFAULT_ROOT), help=f"Input date root. Default: {DEFAULT_ROOT}")
    p.add_argument("--out-dir", type=Path, default=Path(DEFAULT_OUTPUT), help=f"Output directory. Default: {DEFAULT_OUTPUT}")
    p.add_argument("--frames-per-panorama", type=int, default=16, help="Frames per panorama. Default: 16")
    p.add_argument("--expected-frame-size", type=parse_size, default=(4096, 4096), help="Default 4096x4096; use 'any' to disable")
    p.add_argument("--preview-width", type=int, default=8192, help="Preview JPG width. Default: 8192")
    p.add_argument("--preview-quality", type=int, default=95, help="Preview JPG quality 1-100. Default: 95")
    p.add_argument("--no-orient", action="store_true", help="Disable orientation. This writes raw frames directly side by side.")
    p.add_argument("--rotate-only", action="store_true", help="Rotate each raw frame CCW 90 degrees before stitching, but do not horizontally mirror.")
    order = p.add_mutually_exclusive_group()
    order.add_argument("--reverse", dest="reverse", action="store_true", default=True, help="Reverse each 16-frame group before stitching. This is the default.")
    order.add_argument("--forward", dest="reverse", action="store_false", help="Keep each 16-frame group in ascending filename order for comparison")
    p.add_argument("--no-progress", action="store_true", help="Disable terminal progress output")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    manifest = batch_stitch_tree(
        args.root,
        args.out_dir,
        frames_per_panorama=args.frames_per_panorama,
        expected_frame_size=args.expected_frame_size,
        preview_width=args.preview_width,
        preview_quality=args.preview_quality,
        orient=not args.no_orient,
        mirror=not args.rotate_only,
        reverse=args.reverse,
        show_progress=not args.no_progress,
    )
    print(
        "OK "
        f"root={manifest['sourceRoot']} "
        f"out={manifest['outDir']} "
        f"folders={manifest['totalFolders']} "
        f"panoramas={manifest['totalPanoramas']} "
        f"droppedTailFrames={manifest['totalDroppedTailFrames']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
