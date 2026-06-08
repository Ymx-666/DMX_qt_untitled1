#!/usr/bin/env python3
"""Stitch raw recording frames into lossless full panoramas.

Input is one RawRecorder session directory containing index.jsonl plus rgb/ and
bw/ frame folders. Frames are grouped per stream in receive/file order; each
complete group of 16 frames becomes one 65536x4096 panorama by default.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


@dataclass(frozen=True)
class FrameRecord:
    stream: str
    t_ms: int
    file_idx: int
    path: Path


def normalize_stream(stream: str) -> str:
    s = (stream or "").strip().upper()
    if s == "GRAY":
        return "BW"
    if s in ("RGB", "BW"):
        return s
    return "UNK"


def load_index(session_dir: Path) -> List[FrameRecord]:
    session_dir = Path(session_dir)
    index_path = session_dir / "index.jsonl"
    records: List[FrameRecord] = []
    with index_path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            stream = normalize_stream(str(obj.get("stream", "")))
            if stream not in ("RGB", "BW"):
                continue
            rel_file = str(obj.get("file", "")).strip()
            if not rel_file:
                continue
            records.append(
                FrameRecord(
                    stream=stream,
                    t_ms=int(obj.get("t", 0) or 0),
                    file_idx=int(obj.get("fileIdx", 0) or 0),
                    path=session_dir / rel_file,
                )
            )
    return records


def _record_sort_key(record: FrameRecord) -> Tuple[int, int, int, str]:
    if record.file_idx > 0:
        return (0, record.file_idx, record.t_ms, str(record.path))
    return (1, record.t_ms, record.file_idx, str(record.path))


def group_complete_panoramas(
    records: Iterable[FrameRecord],
    frames_per_panorama: int = 16,
) -> Dict[str, List[List[FrameRecord]]]:
    if frames_per_panorama <= 0:
        raise ValueError("frames_per_panorama must be positive")

    by_stream: Dict[str, List[FrameRecord]] = {"RGB": [], "BW": []}
    for record in records:
        if record.stream in by_stream:
            by_stream[record.stream].append(record)

    groups: Dict[str, List[List[FrameRecord]]] = {"RGB": [], "BW": []}
    for stream, stream_records in by_stream.items():
        ordered = sorted(stream_records, key=_record_sort_key)
        full_count = len(ordered) // frames_per_panorama
        groups[stream] = [
            ordered[i * frames_per_panorama : (i + 1) * frames_per_panorama]
            for i in range(full_count)
        ]
    return groups


def parse_size(value: str) -> Optional[Tuple[int, int]]:
    v = value.strip().lower()
    if v in ("any", "none", "0"):
        return None
    if "x" not in v:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT or any")
    w_s, h_s = v.split("x", 1)
    try:
        w = int(w_s)
        h = int(h_s)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT or any") from exc
    if w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("size dimensions must be positive")
    return (w, h)


def _import_cv2():
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore
    except ImportError as exc:
        raise SystemExit("OpenCV/numpy is required: install python3-opencv or opencv-python") from exc
    return cv2, np


def _orient_frame(cv2, image, orient: bool, mirror: bool = True):
    if not orient:
        return image
    rotated = cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    if mirror:
        return cv2.flip(rotated, 1)
    return rotated


def stitch_group(
    group: Sequence[FrameRecord],
    out_path: Path,
    *,
    expected_frame_size: Optional[Tuple[int, int]] = (4096, 4096),
    orient: bool = True,
    mirror: bool = True,
) -> Tuple[int, int, int]:
    if not group:
        raise ValueError("group is empty")

    cv2, np = _import_cv2()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    first = cv2.imread(str(group[0].path), cv2.IMREAD_UNCHANGED)
    if first is None:
        raise RuntimeError(f"read failed: {group[0].path}")
    first = _orient_frame(cv2, first, orient, mirror)
    h, w = first.shape[:2]
    if expected_frame_size is not None and (w, h) != expected_frame_size:
        raise RuntimeError(f"unexpected frame size {w}x{h}: {group[0].path}")

    channels = 1 if len(first.shape) == 2 else first.shape[2]
    if channels == 1:
        pano = np.empty((h, w * len(group)), dtype=first.dtype)
    else:
        pano = np.empty((h, w * len(group), channels), dtype=first.dtype)

    def copy_tile(idx: int, img) -> None:
        if img.shape[:2] != (h, w):
            raise RuntimeError(f"frame size mismatch at tile {idx}: {group[idx].path}")
        x0 = idx * w
        pano[:, x0 : x0 + w] = img

    copy_tile(0, first)
    for idx, record in enumerate(group[1:], 1):
        img = cv2.imread(str(record.path), cv2.IMREAD_UNCHANGED)
        if img is None:
            raise RuntimeError(f"read failed: {record.path}")
        img = _orient_frame(cv2, img, orient, mirror)
        copy_tile(idx, img)

    tiff_compression = getattr(cv2, "IMWRITE_TIFF_COMPRESSION", 259)
    if not cv2.imwrite(str(out_path), pano, [tiff_compression, 5]):
        raise RuntimeError(f"write failed: {out_path}")
    return (pano.shape[1], pano.shape[0], int(pano.size * pano.dtype.itemsize))


def write_preview(src_path: Path, preview_path: Path, preview_width: int, quality: int) -> None:
    cv2, _ = _import_cv2()
    img = cv2.imread(str(src_path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError(f"read failed for preview: {src_path}")
    h, w = img.shape[:2]
    if w <= 0 or h <= 0:
        raise RuntimeError(f"invalid panorama size for preview: {src_path}")
    preview_width = max(1, min(preview_width, w))
    preview_height = max(1, int(h * preview_width / w))
    preview = cv2.resize(img, (preview_width, preview_height), interpolation=cv2.INTER_AREA)
    jpg_quality = getattr(cv2, "IMWRITE_JPEG_QUALITY", 1)
    if not cv2.imwrite(str(preview_path), preview, [jpg_quality, max(1, min(quality, 100))]):
        raise RuntimeError(f"preview write failed: {preview_path}")


def stitch_session(
    session_dir: Path,
    out_dir: Path,
    *,
    frames_per_panorama: int = 16,
    expected_frame_size: Optional[Tuple[int, int]] = (4096, 4096),
    preview_width: int = 8192,
    preview_quality: int = 95,
    orient: bool = True,
    reverse: bool = False,
) -> Dict[str, object]:
    records = load_index(session_dir)
    groups = group_complete_panoramas(records, frames_per_panorama)
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest: Dict[str, object] = {
        "session": str(Path(session_dir).resolve()),
        "framesPerPanorama": frames_per_panorama,
        "expectedFrameSize": "any" if expected_frame_size is None else f"{expected_frame_size[0]}x{expected_frame_size[1]}",
        "orientLikeLivePanorama": orient,
        "reverseOrder": reverse,
        "streams": {},
    }
    streams_obj: Dict[str, object] = {}

    for stream in ("RGB", "BW"):
        items = []
        for group_idx, group in enumerate(groups[stream], 1):
            group_records = list(reversed(group)) if reverse else group
            prefix = stream.lower()
            pano_name = f"{prefix}_pano_{group_idx:04d}.tiff"
            preview_name = f"{prefix}_pano_{group_idx:04d}_preview.jpg"
            pano_path = out_dir / pano_name
            preview_path = out_dir / preview_name
            pano_w, pano_h, bytes_uncompressed = stitch_group(
                group_records,
                pano_path,
                expected_frame_size=expected_frame_size,
                orient=orient,
            )
            write_preview(pano_path, preview_path, preview_width, preview_quality)
            items.append(
                {
                    "file": pano_name,
                    "preview": preview_name,
                    "panoW": pano_w,
                    "panoH": pano_h,
                    "frames": [
                        {
                            "t": r.t_ms,
                            "fileIdx": r.file_idx,
                            "file": str(r.path),
                        }
                        for r in group_records
                    ],
                    "uncompressedBytes": bytes_uncompressed,
                }
            )
        streams_obj[stream] = {
            "panoramas": items,
            "inputFrames": len([r for r in records if r.stream == stream]),
            "droppedTailFrames": len([r for r in records if r.stream == stream]) % frames_per_panorama,
        }

    manifest["streams"] = streams_obj
    with (out_dir / "manifest.json").open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return manifest


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Stitch RawRecorder frames into 65536x4096 lossless panoramas.")
    p.add_argument("session_dir", type=Path, help="RawRecorder session directory containing index.jsonl")
    p.add_argument("--out-dir", type=Path, default=None, help="Output directory; default: SESSION/panoramas")
    p.add_argument("--frames-per-panorama", type=int, default=16, help="Frames per panorama, default 16")
    p.add_argument("--expected-frame-size", type=parse_size, default=(4096, 4096), help="Default 4096x4096; use 'any' to disable")
    p.add_argument("--preview-width", type=int, default=8192, help="Preview JPG width, default 8192")
    p.add_argument("--preview-quality", type=int, default=95, help="Preview JPG quality 1-100")
    p.add_argument("--no-orient", action="store_true", help="Do not rotate/mirror frames before stitching")
    p.add_argument("--reverse", action="store_true", help="Reverse each 16-frame group before stitching")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    session_dir = args.session_dir
    out_dir = args.out_dir if args.out_dir is not None else session_dir / "panoramas"
    manifest = stitch_session(
        session_dir,
        out_dir,
        frames_per_panorama=args.frames_per_panorama,
        expected_frame_size=args.expected_frame_size,
        preview_width=args.preview_width,
        preview_quality=args.preview_quality,
        orient=not args.no_orient,
        reverse=args.reverse,
    )
    streams = manifest["streams"]
    rgb_n = len(streams["RGB"]["panoramas"])  # type: ignore[index]
    bw_n = len(streams["BW"]["panoramas"])  # type: ignore[index]
    print(f"OK out={out_dir} RGB={rgb_n} BW={bw_n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
