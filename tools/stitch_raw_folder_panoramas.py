#!/usr/bin/env python3
"""Stitch one raw image folder into 16-frame lossless panoramas.

Default source:
  smb://tg-ds2309.local/data/raw/20260608/RGB/1153

Default output:
  /home/sht/dmx_data/save

The script accepts either a normal filesystem path or a GNOME/Nautilus smb:// URI.
OpenCV cannot read smb:// directly, so the URI is resolved to the corresponding
/run/user/<uid>/gvfs/smb-share:... path when that share is mounted.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple
from urllib.parse import unquote, urlparse

try:
    from stitch_recording_panoramas import FrameRecord, parse_size, stitch_group, write_preview
except ModuleNotFoundError:
    from .stitch_recording_panoramas import FrameRecord, parse_size, stitch_group, write_preview


DEFAULT_SOURCE = "smb://tg-ds2309.local/data/raw/20260608/RGB/1153"
DEFAULT_OUTPUT = "/home/sht/dmx_data/save"
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff"}


def smb_uri_to_gvfs_candidates(uri: str, uid: Optional[int] = None) -> List[Path]:
    parsed = urlparse(uri)
    if parsed.scheme.lower() != "smb" or not parsed.netloc:
        return []
    if uid is None:
        getuid = getattr(os, "getuid", None)
        uid = int(getuid()) if getuid else 1000

    host = parsed.netloc
    parts = [unquote(p) for p in parsed.path.split("/") if p]
    if not parts:
        return []
    share = parts[0]
    rest = Path(*parts[1:]) if len(parts) > 1 else Path()
    gvfs_root = Path(f"/run/user/{uid}/gvfs")

    return [
        gvfs_root / f"smb-share:server={host},share={share}" / rest,
        gvfs_root / f"smb-share:server={host},share={share.lower()}" / rest,
        gvfs_root / f"smb-share:server={host.lower()},share={share}" / rest,
        gvfs_root / f"smb-share:server={host.lower()},share={share.lower()}" / rest,
    ]


def resolve_source_path(source: str) -> Path:
    if source.lower().startswith("smb://"):
        candidates = smb_uri_to_gvfs_candidates(source)
        for candidate in candidates:
            if candidate.is_dir():
                return candidate
        searched = "\n".join(f"  {p}" for p in candidates)
        raise SystemExit(
            "SMB URI is not visible as a local filesystem path.\n"
            "Open it once in Files/Nautilus or run: gio mount smb://tg-ds2309.local/data\n"
            "Then re-run this script. Checked:\n"
            f"{searched}"
        )

    path = Path(source).expanduser()
    if not path.is_dir():
        raise SystemExit(f"Source directory not found: {path}")
    return path


def dmx_numeric_suffix(path: Path) -> Optional[int]:
    stem = path.stem
    m = re.match(r"^(RGB|BW|GRAY)_(?:.*_)?(\d+)$", stem, re.IGNORECASE)
    if not m:
        return None
    return int(m.group(2))


def dmx_file_sort_key(item: Tuple[int, Path]) -> Tuple[int, str]:
    suffix, path = item
    return (suffix, path.name)


def list_image_files(source_dir: Path) -> List[Path]:
    files: List[Tuple[int, Path]] = []
    for path in Path(source_dir).iterdir():
        if not path.is_file() or path.suffix.lower() not in IMAGE_EXTENSIONS:
            continue
        suffix = dmx_numeric_suffix(path)
        if suffix is None:
            continue
        files.append((suffix, path))
    return [path for _, path in sorted(files, key=dmx_file_sort_key)]


def group_frames(frames: Sequence[Path], frames_per_panorama: int = 16) -> List[List[Path]]:
    if frames_per_panorama <= 0:
        raise ValueError("frames_per_panorama must be positive")
    full_count = len(frames) // frames_per_panorama
    return [
        list(frames[i * frames_per_panorama : (i + 1) * frames_per_panorama])
        for i in range(full_count)
    ]


def stream_name_from_source(source_dir: Path, fallback: str) -> str:
    for part in reversed(source_dir.parts):
        upper = part.upper()
        if upper in ("RGB", "BW", "GRAY"):
            return "BW" if upper == "GRAY" else upper
    return fallback.strip().upper() or "RGB"


def safe_label(value: str, fallback: str = "folder") -> str:
    label = re.sub(r"[^A-Za-z0-9_-]+", "_", value.strip())
    return label.strip("_") or fallback


def folder_label_from_source(source_dir: Path) -> str:
    return safe_label(Path(source_dir).name, "folder")


def stitch_raw_folder(
    source: str,
    out_dir: Path,
    *,
    stream: str = "RGB",
    frames_per_panorama: int = 16,
    expected_frame_size: Optional[Tuple[int, int]] = (4096, 4096),
    preview_width: int = 8192,
    preview_quality: int = 95,
    orient: bool = True,
    mirror: bool = True,
    reverse: bool = True,
) -> dict:
    source_dir = resolve_source_path(source)
    out_dir = Path(out_dir).expanduser()
    out_dir.mkdir(parents=True, exist_ok=True)

    actual_stream = stream_name_from_source(source_dir, stream)
    prefix = actual_stream.lower()
    source_label = folder_label_from_source(source_dir)
    frames = list_image_files(source_dir)
    groups = group_frames(frames, frames_per_panorama)

    manifest = {
        "source": source,
        "resolvedSource": str(source_dir),
        "outDir": str(out_dir),
        "stream": actual_stream,
        "sourceLabel": source_label,
        "framesPerPanorama": frames_per_panorama,
        "inputFrames": len(frames),
        "panoramaCount": len(groups),
        "droppedTailFrames": len(frames) % frames_per_panorama,
        "expectedFrameSize": "any" if expected_frame_size is None else f"{expected_frame_size[0]}x{expected_frame_size[1]}",
        "orientLikeLivePanorama": orient and mirror,
        "orientation": (
            "rotate_ccw_90_then_horizontal_mirror"
            if orient and mirror else
            "rotate_ccw_90_only"
            if orient else
            "none"
        ),
        "reverseOrder": reverse,
        "panoramas": [],
    }

    for group_idx, group_paths in enumerate(groups, 1):
        ordered_paths = list(reversed(group_paths)) if reverse else group_paths
        records = [
            FrameRecord(actual_stream, 0, idx + 1, path)
            for idx, path in enumerate(ordered_paths)
        ]
        pano_name = f"{prefix}_{source_label}_pano_{group_idx:04d}.tiff"
        preview_name = f"{prefix}_{source_label}_pano_{group_idx:04d}_preview.jpg"
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
        manifest["panoramas"].append(
            {
                "file": pano_name,
                "preview": preview_name,
                "panoW": pano_w,
                "panoH": pano_h,
                "frames": [str(p) for p in ordered_paths],
                "uncompressedBytes": uncompressed_bytes,
            }
        )

    manifest_path = out_dir / "raw_folder_stitch_manifest.json"
    with manifest_path.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return manifest


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Stitch a raw image folder into 16-frame panoramas.")
    p.add_argument("--src", default=DEFAULT_SOURCE, help=f"Source folder or smb:// URI. Default: {DEFAULT_SOURCE}")
    p.add_argument("--out-dir", type=Path, default=Path(DEFAULT_OUTPUT), help=f"Output directory. Default: {DEFAULT_OUTPUT}")
    p.add_argument("--stream", default="RGB", help="Stream name used for output file prefix when not inferable. Default: RGB")
    p.add_argument("--frames-per-panorama", type=int, default=16, help="Frames per panorama. Default: 16")
    p.add_argument("--expected-frame-size", type=parse_size, default=(4096, 4096), help="Default 4096x4096; use 'any' to disable")
    p.add_argument("--preview-width", type=int, default=8192, help="Preview JPG width. Default: 8192")
    p.add_argument("--preview-quality", type=int, default=95, help="Preview JPG quality 1-100. Default: 95")
    p.add_argument("--no-orient", action="store_true", help="Disable orientation. This writes raw frames directly side by side.")
    p.add_argument("--rotate-only", action="store_true", help="Rotate each raw frame CCW 90 degrees before stitching, but do not horizontally mirror.")
    order = p.add_mutually_exclusive_group()
    order.add_argument("--reverse", dest="reverse", action="store_true", default=True, help="Reverse each 16-frame group before stitching. This is the default.")
    order.add_argument("--forward", dest="reverse", action="store_false", help="Keep each 16-frame group in ascending filename order for comparison")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    manifest = stitch_raw_folder(
        args.src,
        args.out_dir,
        stream=args.stream,
        frames_per_panorama=args.frames_per_panorama,
        expected_frame_size=args.expected_frame_size,
        preview_width=args.preview_width,
        preview_quality=args.preview_quality,
        orient=not args.no_orient,
        mirror=not args.rotate_only,
        reverse=args.reverse,
    )
    print(
        "OK "
        f"source={manifest['resolvedSource']} "
        f"out={manifest['outDir']} "
        f"panoramas={manifest['panoramaCount']} "
        f"droppedTailFrames={manifest['droppedTailFrames']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
