#!/usr/bin/env python3
"""Block2All: merge raw block frames into half-panorama JPGs.

Usage:
  block2all SRC_DIR OUT_ROOT HALF_COUNT [OUTPUT_COUNT]
  python3 tools/block2all.py SRC_DIR OUT_ROOT HALF_COUNT [OUTPUT_COUNT]

When launched without arguments, a small Tkinter GUI is shown.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff"}
DEFAULT_SOURCE_ROOT = "/mnt/dmx4t/data_all/data"
DEFAULT_OUTPUT_ROOT = "/mnt/dmx4t/data_all"
JPEG_QUALITY = 95


@dataclass(frozen=True)
class BlockImage:
    path: Path
    index: int


@dataclass(frozen=True)
class RecordingFrame:
    stream: str
    t_ms: int
    file_idx: int
    path: Path


@dataclass(frozen=True)
class RawDataFrame:
    stream: str
    day: str
    time_text: str
    file_idx: int
    path: Path


@dataclass
class HalfPanoramaResult:
    file: str
    group: int
    half: str
    frames: List[str]
    width: int
    height: int


@dataclass
class SkippedHalfPanorama:
    file: str
    group: int
    half: str
    reason: str


@dataclass
class FailedPanoramaGroup:
    group: int
    halves: List[str]
    frames: List[str]
    error: str


def trailing_index(path: Path) -> Optional[int]:
    match = re.search(r"(\d+)$", path.stem)
    if not match:
        return None
    return int(match.group(1))


def list_block_images(src_dir: Path) -> List[BlockImage]:
    src_dir = Path(src_dir)
    if not src_dir.is_dir():
        raise RuntimeError(f"原始分块图像路径不存在: {src_dir}")

    images: List[BlockImage] = []
    for path in src_dir.iterdir():
        if not path.is_file() or path.suffix.lower() not in IMAGE_EXTENSIONS:
            continue
        idx = trailing_index(path)
        if idx is None:
            continue
        images.append(BlockImage(path=path, index=idx))

    images.sort(key=lambda item: (item.index, item.path.name))
    return images


def normalize_stream(stream: str) -> str:
    value = (stream or "").strip().upper()
    if value == "GRAY":
        return "BW"
    if value in ("RGB", "BW"):
        return value
    return "UNK"


def load_recording_index(session_dir: Path) -> List[RecordingFrame]:
    session_dir = Path(session_dir)
    index_path = session_dir / "index.jsonl"
    if not index_path.is_file():
        raise RuntimeError(f"录制索引不存在: {index_path}")

    frames: List[RecordingFrame] = []
    with index_path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"索引 JSON 解析失败: {index_path}:{line_no}") from exc
            stream = normalize_stream(str(obj.get("stream", "")))
            if stream not in ("RGB", "BW"):
                continue
            rel_file = str(obj.get("file", "")).strip()
            if not rel_file:
                continue
            path = session_dir / rel_file
            if not path.is_file():
                continue
            frames.append(
                RecordingFrame(
                    stream=stream,
                    t_ms=int(obj.get("t", 0) or 0),
                    file_idx=int(obj.get("fileIdx", 0) or 0),
                    path=path,
                )
            )
    return frames


def recording_sort_key(frame: RecordingFrame) -> Tuple[int, int, int, str]:
    if frame.file_idx > 0:
        return (0, frame.file_idx, frame.t_ms, str(frame.path))
    return (1, frame.t_ms, frame.file_idx, str(frame.path))


def parse_raw_data_frame(path: Path) -> Optional[RawDataFrame]:
    match = re.match(r"^(BW|RGB)_(\d{8})_(\d{6})_(\d+)$", path.stem, re.IGNORECASE)
    if not match:
        return None
    stream = normalize_stream(match.group(1))
    if stream not in ("BW", "RGB"):
        return None
    return RawDataFrame(
        stream=stream,
        day=match.group(2),
        time_text=match.group(3),
        file_idx=int(match.group(4)),
        path=path,
    )


def raw_data_sort_key(frame: RawDataFrame) -> Tuple[str, str, int, str]:
    return (frame.day, frame.time_text, frame.file_idx, str(frame.path))


def discover_raw_data_stream_dirs(src_dir: Path) -> List[Tuple[str, str, Path]]:
    src_dir = Path(src_dir)
    if not src_dir.is_dir():
        raise RuntimeError(f"原始数据路径不存在: {src_dir}")

    name = src_dir.name.upper()
    parent_name = src_dir.parent.name
    if name in ("BW", "RGB") and re.fullmatch(r"\d{8}", parent_name):
        return [(parent_name, name, src_dir)]

    if re.fullmatch(r"\d{8}", src_dir.name):
        candidates = [src_dir]
    else:
        candidates = [p for p in src_dir.iterdir() if p.is_dir() and re.fullmatch(r"\d{8}", p.name)]
        candidates.sort(key=lambda p: p.name)

    stream_dirs: List[Tuple[str, str, Path]] = []
    for day_dir in candidates:
        for stream in ("BW", "RGB"):
            stream_dir = day_dir / stream
            if stream_dir.is_dir():
                stream_dirs.append((day_dir.name, stream, stream_dir))
            lower_stream_dir = day_dir / stream.lower()
            if lower_stream_dir.is_dir():
                stream_dirs.append((day_dir.name, stream, lower_stream_dir))
    return stream_dirs


def discover_recording_sessions(src_dir: Path) -> List[Path]:
    src_dir = Path(src_dir)
    if not src_dir.is_dir():
        raise RuntimeError(f"录制数据路径不存在: {src_dir}")
    if (src_dir / "index.jsonl").is_file():
        return [src_dir]
    sessions = [
        p for p in src_dir.iterdir()
        if p.is_dir() and (p / "index.jsonl").is_file()
    ]
    sessions.sort(key=lambda p: p.name)
    return sessions


def infer_session_name(first_file: Path) -> str:
    # Prefer names like BW_20260616_134920_1013.jpg -> ALL2_20260616_134920.
    match = re.match(r"^[A-Za-z]+_(\d{8})_(\d{6})_\d+$", first_file.stem)
    if match:
        return f"ALL2_{match.group(1)}_{match.group(2)}"
    return datetime.now().strftime("ALL2_%Y%m%d_%H%M%S")


def recording_session_datetime(session_dir: Path, frames: Sequence[RecordingFrame]) -> datetime:
    match = re.match(r"^REC2_(\d{8})_(\d{6})$", session_dir.name)
    if match:
        return datetime.strptime(match.group(1) + match.group(2), "%Y%m%d%H%M%S")

    match = re.match(r"^REC2_(\d{13})$", session_dir.name)
    if match:
        return datetime.fromtimestamp(int(match.group(1)) / 1000.0)

    frame_times = [frame.t_ms for frame in frames if frame.t_ms > 0]
    if frame_times:
        return datetime.fromtimestamp(min(frame_times) / 1000.0)

    return datetime.now()


def recording_session_output_base(out_root: Path, session_dir: Path, frames: Sequence[RecordingFrame]) -> Path:
    session_time = recording_session_datetime(session_dir, frames)
    day = session_time.strftime("%Y%m%d")
    session_name = session_time.strftime("ALL2_%Y%m%d_%H%M%S")
    return Path(out_root) / day / session_name


def unique_output_dir(out_root: Path, first_file: Path) -> Path:
    out_root = Path(out_root)
    base = out_root / infer_session_name(first_file)
    if not base.exists():
        return base
    for i in range(2, 10000):
        candidate = out_root / f"{base.name}_{i:02d}"
        if not candidate.exists():
            return candidate
    raise RuntimeError(f"无法创建唯一输出目录: {out_root}")


def output_name(first_frame: Path, suffix: str) -> str:
    return f"{first_frame.stem}-{suffix}.jpg"


def recording_output_name(stream: str, first_frame: Path, index: int, suffix: str) -> str:
    return f"{stream}_{first_frame.stem}_{index:06d}-{suffix}.jpg"


def _import_cv2():
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore
    except ImportError as exc:
        raise RuntimeError("缺少 OpenCV/numpy，请安装 python3-opencv python3-numpy") from exc
    return cv2, np


def orient_frame(cv2, image):
    rotated = cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return cv2.flip(rotated, 1)


def stitch_half(paths: Sequence[Path]):
    if not paths:
        raise RuntimeError("半全景帧列表为空")

    cv2, np = _import_cv2()
    oriented = []
    width = 0
    height = 0

    for path in reversed(list(paths)):
        img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if img is None:
            raise RuntimeError(f"读取失败: {path}")
        img = orient_frame(cv2, img)
        if len(img.shape) == 2:
            img = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
        elif img.shape[2] == 4:
            img = cv2.cvtColor(img, cv2.COLOR_BGRA2BGR)
        h, w = img.shape[:2]
        if not oriented:
            width = w
            height = h
        elif w != width or h != height:
            raise RuntimeError(f"图像尺寸不一致: {path} ({w}x{h})，期望 {width}x{height}")
        oriented.append(img)

    return np.hstack(oriented)


def write_jpg(path: Path, image) -> None:
    cv2, _ = _import_cv2()
    path.parent.mkdir(parents=True, exist_ok=True)
    quality_flag = getattr(cv2, "IMWRITE_JPEG_QUALITY", 1)
    if not cv2.imwrite(str(path), image, [quality_flag, JPEG_QUALITY]):
        raise RuntimeError(f"写入失败: {path}")


def shape_hw(image) -> Tuple[int, int]:
    h, w = image.shape[:2]
    return h, w


def build_half_jobs_from_paths(paths: Sequence[Path], half_count: int):
    jobs = []
    for half_idx in range(len(paths) // half_count):
        half = list(paths[half_idx * half_count : (half_idx + 1) * half_count])
        suffix = "A" if (half_idx % 2) == 0 else "B"
        jobs.append((half_idx + 1, suffix, half[0], half))
    return jobs


def build_grouped_half_jobs_from_paths(paths: Sequence[Path], half_count: int):
    jobs = []
    full_group_size = half_count * 2
    full_groups = len(paths) // full_group_size
    tail_frames = len(paths) % full_group_size
    for group_idx in range(full_groups):
        group_start = group_idx * full_group_size
        group = list(paths[group_start : group_start + full_group_size])
        base_frame = group[0]
        jobs.append((group_idx + 1, "A", base_frame, group[:half_count]))
        jobs.append((group_idx + 1, "B", base_frame, group[half_count:full_group_size]))
    if tail_frames >= half_count:
        group_start = full_groups * full_group_size
        half = list(paths[group_start : group_start + half_count])
        jobs.append((full_groups + 1, "A", half[0], half))
    return jobs


def process_half_jobs(
    half_jobs,
    out_dir: Path,
    *,
    output_count: Optional[int],
    progress: Optional[Callable[[str], None]],
    naming: Callable[[int, str, Path], str],
) -> Tuple[List[HalfPanoramaResult], int, int, int]:
    def log(message: str) -> None:
        if progress:
            progress(message)

    available_outputs = len(half_jobs)
    shortage = 0
    selected_jobs = list(half_jobs)
    if output_count is not None and output_count < available_outputs:
        selected_jobs = selected_jobs[:output_count]
    elif output_count is not None and output_count > available_outputs:
        shortage = output_count - available_outputs
        log(f"数量不足: 目标 {output_count} 张，当前数据只能生成 {available_outputs} 张")

    results: List[HalfPanoramaResult] = []
    for idx, suffix, base_frame, paths in selected_jobs:
        out_path = out_dir / naming(idx, suffix, base_frame)
        log(f"生成 {out_path.name}: {paths[-1].name} ... {paths[0].name}")
        pano = stitch_half(paths)
        write_jpg(out_path, pano)
        h, w = pano.shape[:2]
        results.append(
            HalfPanoramaResult(
                file=out_path.name,
                group=idx,
                half=suffix,
                frames=[str(p) for p in reversed(paths)],
                width=w,
                height=h,
            )
        )

    unprocessed_by_limit = max(0, (available_outputs - len(results)) * len(selected_jobs[0][3]) if selected_jobs else 0)
    return results, available_outputs, shortage, unprocessed_by_limit



def process_grouped_half_jobs(
    half_jobs,
    *,
    progress: Optional[Callable[[str], None]],
    make_output_path: Callable[[int, str, Path], Path],
    manifest_file: Callable[[Path], str],
    log_prefix: str = "",
) -> Tuple[List[HalfPanoramaResult], List[SkippedHalfPanorama], List[FailedPanoramaGroup]]:
    def log(message: str) -> None:
        if progress:
            progress(message)

    groups = []
    current_key = None
    current = []
    for job in half_jobs:
        key = job[0]
        if current and key != current_key:
            groups.append(current)
            current = []
        current_key = key
        current.append(job)
    if current:
        groups.append(current)

    results: List[HalfPanoramaResult] = []
    skipped: List[SkippedHalfPanorama] = []
    failed: List[FailedPanoramaGroup] = []

    for group_jobs in groups:
        prepared = []
        for group_idx, suffix, base_frame, paths in group_jobs:
            out_path = make_output_path(group_idx, suffix, base_frame)
            prepared.append((group_idx, suffix, base_frame, list(paths), out_path))

        if prepared and all(out_path.exists() for _, _, _, _, out_path in prepared):
            for group_idx, suffix, _, _, out_path in prepared:
                skipped.append(
                    SkippedHalfPanorama(
                        file=manifest_file(out_path),
                        group=group_idx,
                        half=suffix,
                        reason="output_exists",
                    )
                )
            names = ", ".join(out_path.name for _, _, _, _, out_path in prepared)
            log(f"跳过已存在: {log_prefix}{names}")
            continue

        stitched = []
        try:
            for group_idx, suffix, _, paths, out_path in prepared:
                log(f"生成 {log_prefix}{out_path.name}: {paths[-1].name} ... {paths[0].name}")
                pano = stitch_half(paths)
                h, w = shape_hw(pano)
                stitched.append((group_idx, suffix, paths, out_path, pano, h, w))
        except Exception as exc:  # noqa: BLE001 - keep batch processing subsequent panoramas.
            failed.append(
                FailedPanoramaGroup(
                    group=prepared[0][0] if prepared else 0,
                    halves=[suffix for _, suffix, _, _, _ in prepared],
                    frames=[str(p) for _, _, _, paths, _ in prepared for p in reversed(paths)],
                    error=str(exc),
                )
            )
            names = ", ".join(out_path.name for _, _, _, _, out_path in prepared)
            log(f"跳过整组 {log_prefix}{names}: {exc}")
            continue

        for group_idx, suffix, paths, out_path, pano, h, w in stitched:
            if out_path.exists():
                skipped.append(
                    SkippedHalfPanorama(
                        file=manifest_file(out_path),
                        group=group_idx,
                        half=suffix,
                        reason="output_exists",
                    )
                )
                log(f"跳过已存在: {log_prefix}{out_path.name}")
                continue
            write_jpg(out_path, pano)
            results.append(
                HalfPanoramaResult(
                    file=manifest_file(out_path),
                    group=group_idx,
                    half=suffix,
                    frames=[str(p) for p in reversed(paths)],
                    width=w,
                    height=h,
                )
            )

    return results, skipped, failed

def block2all(
    src_dir: Path,
    out_root: Path,
    half_count: int,
    *,
    output_count: Optional[int] = None,
    progress: Optional[Callable[[str], None]] = None,
) -> Dict[str, object]:
    if half_count <= 0:
        raise RuntimeError("合并半全景图像的数目必须大于 0")
    if output_count is not None and output_count <= 0:
        raise RuntimeError("要生成的全景图数量必须大于 0")

    def log(message: str) -> None:
        if progress:
            progress(message)

    blocks = list_block_images(src_dir)
    if len(blocks) < half_count:
        raise RuntimeError(f"有效图像数量不足: {len(blocks)}，至少需要 {half_count}")

    out_dir = unique_output_dir(Path(out_root), blocks[0].path)
    out_dir.mkdir(parents=True, exist_ok=True)

    full_group_size = half_count * 2
    full_groups = len(blocks) // full_group_size
    tail_frames = len(blocks) % full_group_size
    results: List[HalfPanoramaResult] = []

    log(f"输入: {Path(src_dir).resolve()}")
    log(f"输出: {out_dir}")
    log(f"有效图像: {len(blocks)}，每半全景 {half_count} 张，每组 {full_group_size} 张")
    if output_count is not None:
        log(f"目标输出全景图: {output_count} 张")

    half_jobs = []
    for group_idx in range(full_groups):
        group_start = group_idx * full_group_size
        group = blocks[group_start : group_start + full_group_size]
        base_frame = group[0].path
        half_jobs.append((group_idx + 1, "A", base_frame, group[:half_count]))
        half_jobs.append((group_idx + 1, "B", base_frame, group[half_count:full_group_size]))

    if tail_frames >= half_count:
        group_start = full_groups * full_group_size
        half = blocks[group_start : group_start + half_count]
        base_frame = half[0].path
        half_jobs.append((full_groups + 1, "A", base_frame, half))

    available_outputs = len(half_jobs)
    shortage = 0
    if output_count is not None and output_count < available_outputs:
        half_jobs = half_jobs[:output_count]
    elif output_count is not None and output_count > available_outputs:
        shortage = output_count - available_outputs
        log(f"数量不足: 目标 {output_count} 张，当前数据只能生成 {available_outputs} 张")

    half_jobs = [
        (group_idx, suffix, base_frame, [item.path for item in half])
        for group_idx, suffix, base_frame, half in half_jobs
    ]
    selected_output_images = len(half_jobs)
    results, skipped, failed = process_grouped_half_jobs(
        half_jobs,
        progress=progress,
        make_output_path=lambda _idx, suffix, base_frame: out_dir / output_name(base_frame, suffix),
        manifest_file=lambda path: path.name,
    )

    dropped_tail_frames = max(0, len(blocks) - available_outputs * half_count)
    unprocessed_by_limit = max(0, (available_outputs - selected_output_images) * half_count)

    manifest = {
        "tool": "Block2All",
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "sourceDir": str(Path(src_dir).resolve()),
        "outDir": str(out_dir),
        "halfCount": half_count,
        "requestedOutputImages": output_count,
        "availableOutputImages": available_outputs,
        "jpegQuality": JPEG_QUALITY,
        "inputFrames": len(blocks),
        "outputImages": len(results),
        "skippedOutputImages": len(skipped),
        "failedPanoramaGroups": len(failed),
        "shortageOutputImages": shortage,
        "droppedTailFrames": dropped_tail_frames,
        "unprocessedFramesByLimit": unprocessed_by_limit,
        "orientation": "rotate_ccw_90_then_horizontal_mirror",
        "order": "each half panorama is stitched in reverse numeric order",
        "outputs": [r.__dict__ for r in results],
        "skipped": [r.__dict__ for r in skipped],
        "failed": [r.__dict__ for r in failed],
    }
    with (out_dir / "manifest.json").open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    log(f"完成: 输出 {len(results)} 张，跳过已存在 {len(skipped)} 张，失败全景组 {len(failed)} 个，丢弃尾帧 {dropped_tail_frames} 张，因数量限制未处理 {unprocessed_by_limit} 张")
    return manifest


def block2all_recordings(
    src_dir: Path,
    out_root: Path,
    half_count: int,
    *,
    output_count: Optional[int] = None,
    stream: str = "BOTH",
    progress: Optional[Callable[[str], None]] = None,
) -> Dict[str, object]:
    if half_count <= 0:
        raise RuntimeError("合并半全景图像的数目必须大于 0")
    if output_count is not None and output_count <= 0:
        raise RuntimeError("要生成的全景图数量必须大于 0")

    def log(message: str) -> None:
        if progress:
            progress(message)

    sessions = discover_recording_sessions(src_dir)
    if not sessions:
        raise RuntimeError(f"没有找到录制会话 index.jsonl: {src_dir}")

    stream = stream.strip().upper()
    if stream not in ("RGB", "BW", "BOTH"):
        stream = "BOTH"
    streams = ["BW", "RGB"] if stream == "BOTH" else [stream]

    plan = []
    available_outputs = 0
    input_frames = 0
    dropped_tail_frames = 0
    for session in sessions:
        frames = load_recording_index(session)
        for one_stream in streams:
            stream_frames = sorted([f for f in frames if f.stream == one_stream], key=recording_sort_key)
            input_frames += len(stream_frames)
            dropped_tail_frames += len(stream_frames) % half_count
            paths = [f.path for f in stream_frames]
            jobs = build_half_jobs_from_paths(paths, half_count)
            if jobs:
                session_out_base = recording_session_output_base(out_root, session, frames)
                plan.append((session, session_out_base, one_stream, jobs))
                available_outputs += len(jobs)

    if available_outputs <= 0:
        raise RuntimeError(f"有效图像数量不足，无法按每 {half_count} 帧生成半全景")

    out_root = Path(out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    log(f"输入: {Path(src_dir).resolve()}")
    log(f"输出根目录: {out_root}")
    log(f"录制会话: {len(sessions)} 个，流: {','.join(streams)}")
    log(f"有效输入帧: {input_frames}，每半全景 {half_count} 张，可生成 {available_outputs} 张")
    if output_count is not None:
        log(f"目标输出全景图: {output_count} 张")

    shortage = 0
    remaining = output_count
    if output_count is not None and output_count > available_outputs:
        shortage = output_count - available_outputs
        log(f"数量不足: 目标 {output_count} 张，当前录制数据只能生成 {available_outputs} 张")

    outputs = []
    skipped_outputs = []
    failed_groups = []
    selected_output_images = 0
    manifest_sessions = []
    for session, session_out_base, one_stream, jobs in plan:
        if remaining is not None and remaining <= 0:
            break
        selected = jobs if remaining is None else jobs[:remaining]
        if not selected:
            continue
        selected_output_images += len(selected)
        session_out_dir = session_out_base / one_stream.lower()
        session_out_dir.mkdir(parents=True, exist_ok=True)
        log(f"处理会话 {session.name} / {one_stream}: {len(selected)} 张 -> {session_out_dir}")

        session_results, session_skipped, session_failed = process_grouped_half_jobs(
            selected,
            progress=progress,
            make_output_path=lambda idx, suffix, base_frame, one_stream=one_stream, session_out_dir=session_out_dir: session_out_dir / recording_output_name(one_stream, base_frame, idx, suffix),
            manifest_file=lambda path, out_root=out_root: str(path.relative_to(out_root)),
            log_prefix=f"{session.name}/{one_stream.lower()}/",
        )
        outputs.extend(session_results)
        skipped_outputs.extend(session_skipped)
        failed_groups.extend(session_failed)
        manifest_sessions.append(
            {
                "session": str(session),
                "stream": one_stream,
                "allDir": str(session_out_base),
                "outDir": str(session_out_dir),
                "outputImages": len(session_results),
                "skippedOutputImages": len(session_skipped),
                "failedPanoramaGroups": len(session_failed),
                "outputs": [item.__dict__ for item in session_results],
                "skipped": [item.__dict__ for item in session_skipped],
                "failed": [item.__dict__ for item in session_failed],
            }
        )
        if remaining is not None:
            remaining -= len(selected)

    unprocessed_by_limit = max(0, (available_outputs - selected_output_images) * half_count)
    manifest = {
        "tool": "Block2All",
        "mode": "recordings",
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "sourceDir": str(Path(src_dir).resolve()),
        "outDir": str(out_root),
        "halfCount": half_count,
        "stream": stream,
        "requestedOutputImages": output_count,
        "availableOutputImages": available_outputs,
        "jpegQuality": JPEG_QUALITY,
        "inputFrames": input_frames,
        "outputImages": len(outputs),
        "skippedOutputImages": len(skipped_outputs),
        "failedPanoramaGroups": len(failed_groups),
        "shortageOutputImages": shortage,
        "droppedTailFrames": dropped_tail_frames,
        "unprocessedFramesByLimit": unprocessed_by_limit,
        "orientation": "rotate_ccw_90_then_horizontal_mirror",
        "order": "each half panorama is stitched in reverse numeric order",
        "sessions": manifest_sessions,
    }
    manifest_path = out_root / "block2all_recordings_manifest.json"
    with manifest_path.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    log(f"完成: 输出 {len(outputs)} 张，跳过已存在 {len(skipped_outputs)} 张，失败全景组 {len(failed_groups)} 个，丢弃尾帧 {dropped_tail_frames} 张，因数量限制未处理 {unprocessed_by_limit} 张")
    return manifest


def block2all_data_tree(
    src_dir: Path,
    out_root: Path,
    half_count: int,
    *,
    output_count: Optional[int] = None,
    stream: str = "BOTH",
    progress: Optional[Callable[[str], None]] = None,
) -> Dict[str, object]:
    if half_count <= 0:
        raise RuntimeError("合并半全景图像的数目必须大于 0")
    if output_count is not None and output_count <= 0:
        raise RuntimeError("要生成的全景图数量必须大于 0")

    def log(message: str) -> None:
        if progress:
            progress(message)

    stream = stream.strip().upper()
    if stream not in ("RGB", "BW", "BOTH"):
        stream = "BOTH"
    wanted_streams = {"BW", "RGB"} if stream == "BOTH" else {stream}

    stream_dirs = [
        item for item in discover_raw_data_stream_dirs(src_dir)
        if item[1] in wanted_streams
    ]
    if not stream_dirs:
        raise RuntimeError(f"没有找到日期/BW 或 日期/RGB 原始图像目录: {src_dir}")

    out_root = Path(out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    plan = []
    available_outputs = 0
    input_frames = 0
    dropped_tail_frames = 0
    for day, one_stream, stream_dir in stream_dirs:
        frames: List[RawDataFrame] = []
        for path in stream_dir.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in IMAGE_EXTENSIONS:
                continue
            frame = parse_raw_data_frame(path)
            if frame is None or frame.stream != one_stream or frame.day != day:
                continue
            frames.append(frame)
        frames.sort(key=raw_data_sort_key)
        input_frames += len(frames)
        dropped_tail_frames += len(frames) % half_count
        jobs = build_grouped_half_jobs_from_paths([f.path for f in frames], half_count)
        if jobs:
            plan.append((day, one_stream, stream_dir, jobs))
            available_outputs += len(jobs)

    if available_outputs <= 0:
        raise RuntimeError(f"有效图像数量不足，无法按每 {half_count} 帧生成半全景")

    log(f"输入: {Path(src_dir).resolve()}")
    log(f"输出根目录: {out_root}")
    log(f"原始数据目录: {len(stream_dirs)} 个，流: {','.join(sorted(wanted_streams))}")
    log(f"有效输入帧: {input_frames}，每半全景 {half_count} 张，可生成 {available_outputs} 张")
    if output_count is not None:
        log(f"目标输出全景图: {output_count} 张")

    shortage = 0
    remaining = output_count
    if output_count is not None and output_count > available_outputs:
        shortage = output_count - available_outputs
        log(f"数量不足: 目标 {output_count} 张，当前原始数据只能生成 {available_outputs} 张")

    outputs = []
    skipped_outputs = []
    failed_groups = []
    selected_output_images = 0
    manifest_streams = []
    for day, one_stream, stream_dir, jobs in plan:
        if remaining is not None and remaining <= 0:
            break
        selected = jobs if remaining is None else jobs[:remaining]
        if not selected:
            continue
        selected_output_images += len(selected)

        log(f"处理 {day}/{one_stream}: {len(selected)} 张")

        def make_data_output_path(_idx: int, suffix: str, base_frame: Path, *, day=day, one_stream=one_stream) -> Path:
            base_info = parse_raw_data_frame(base_frame)
            if base_info is None:
                raise RuntimeError(f"无法解析原始图像文件名: {base_frame}")
            hour = base_info.time_text[:2]
            return out_root / day / one_stream.lower() / hour / output_name(base_frame, suffix)

        stream_results, stream_skipped, stream_failed = process_grouped_half_jobs(
            selected,
            progress=progress,
            make_output_path=make_data_output_path,
            manifest_file=lambda path, out_root=out_root: str(path.relative_to(out_root)),
            log_prefix=f"{day}/{one_stream.lower()}/",
        )
        outputs.extend(stream_results)
        skipped_outputs.extend(stream_skipped)
        failed_groups.extend(stream_failed)
        manifest_streams.append(
            {
                "day": day,
                "stream": one_stream,
                "sourceDir": str(stream_dir),
                "outDir": str(out_root / day / one_stream.lower()),
                "outputImages": len(stream_results),
                "skippedOutputImages": len(stream_skipped),
                "failedPanoramaGroups": len(stream_failed),
                "outputs": [item.__dict__ for item in stream_results],
                "skipped": [item.__dict__ for item in stream_skipped],
                "failed": [item.__dict__ for item in stream_failed],
            }
        )
        if remaining is not None:
            remaining -= len(selected)

    unprocessed_by_limit = max(0, (available_outputs - selected_output_images) * half_count)
    manifest = {
        "tool": "Block2All",
        "mode": "data_tree",
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "sourceDir": str(Path(src_dir).resolve()),
        "outDir": str(out_root),
        "halfCount": half_count,
        "stream": stream,
        "requestedOutputImages": output_count,
        "availableOutputImages": available_outputs,
        "jpegQuality": JPEG_QUALITY,
        "inputFrames": input_frames,
        "outputImages": len(outputs),
        "skippedOutputImages": len(skipped_outputs),
        "failedPanoramaGroups": len(failed_groups),
        "shortageOutputImages": shortage,
        "droppedTailFrames": dropped_tail_frames,
        "unprocessedFramesByLimit": unprocessed_by_limit,
        "orientation": "rotate_ccw_90_then_horizontal_mirror",
        "order": "each half panorama is stitched in reverse numeric order",
        "streams": manifest_streams,
    }
    manifest_path = out_root / "block2all_data_manifest.json"
    with manifest_path.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    log(f"完成: 输出 {len(outputs)} 张，跳过已存在 {len(skipped_outputs)} 张，失败全景组 {len(failed_groups)} 个，丢弃尾帧 {dropped_tail_frames} 张，因数量限制未处理 {unprocessed_by_limit} 张")
    return manifest


def block2all_auto(
    src_dir: Path,
    out_root: Path,
    half_count: int,
    *,
    output_count: Optional[int] = None,
    stream: str = "BOTH",
    progress: Optional[Callable[[str], None]] = None,
) -> Dict[str, object]:
    src_dir = Path(src_dir)
    if (src_dir / "index.jsonl").is_file():
        return block2all_recordings(src_dir, out_root, half_count, output_count=output_count, stream=stream, progress=progress)
    try:
        sessions = discover_recording_sessions(src_dir)
    except RuntimeError:
        sessions = []
    if sessions:
        return block2all_recordings(src_dir, out_root, half_count, output_count=output_count, stream=stream, progress=progress)
    return block2all_data_tree(src_dir, out_root, half_count, output_count=output_count, stream=stream, progress=progress)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="block2all",
        description="合并原始分块图像为 JPG 半全景图像。无参数启动时打开图形界面。",
    )
    parser.add_argument("src_dir", nargs="?", help="原始数据路径，例如 /mnt/dmx4t/data_all/data 或其中某一天目录")
    parser.add_argument("out_root", nargs="?", help="目的根路径，例如 /mnt/dmx4t/data_all")
    parser.add_argument("half_count", nargs="?", type=int, help="合并半全景图像的数目，例如 8")
    parser.add_argument("output_count", nargs="?", type=int, help="需要生成的全景图张数；不填则处理全部")
    parser.add_argument("--stream", choices=["BW", "RGB", "BOTH"], default="BOTH", help="处理录制流：BW、RGB 或 BOTH。默认 BOTH")
    return parser


def run_gui() -> int:
    import threading
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("Block2All 图像合并")
    root.geometry("760x460")

    src_var = tk.StringVar(value=DEFAULT_SOURCE_ROOT)
    out_var = tk.StringVar(value=DEFAULT_OUTPUT_ROOT)
    count_var = tk.StringVar(value="8")
    output_count_var = tk.StringVar(value="")
    stream_var = tk.StringVar(value="BOTH")
    running = {"value": False}

    frame = ttk.Frame(root, padding=14)
    frame.pack(fill=tk.BOTH, expand=True)
    frame.columnconfigure(1, weight=1)

    ttk.Label(frame, text="原始数据路径").grid(row=0, column=0, sticky="w", pady=5)
    ttk.Entry(frame, textvariable=src_var).grid(row=0, column=1, sticky="ew", pady=5)
    ttk.Button(frame, text="选择", command=lambda: choose_dir(src_var)).grid(row=0, column=2, padx=6)

    ttk.Label(frame, text="目的路径").grid(row=1, column=0, sticky="w", pady=5)
    ttk.Entry(frame, textvariable=out_var).grid(row=1, column=1, sticky="ew", pady=5)
    ttk.Button(frame, text="选择", command=lambda: choose_dir(out_var)).grid(row=1, column=2, padx=6)

    ttk.Label(frame, text="每半全景图像帧数").grid(row=2, column=0, sticky="w", pady=5)
    ttk.Entry(frame, textvariable=count_var, width=12).grid(row=2, column=1, sticky="w", pady=5)

    ttk.Label(frame, text="要生成全景图数量").grid(row=3, column=0, sticky="w", pady=5)
    ttk.Entry(frame, textvariable=output_count_var, width=12).grid(row=3, column=1, sticky="w", pady=5)
    ttk.Label(frame, text="留空表示全部处理").grid(row=3, column=1, sticky="w", padx=(110, 0), pady=5)

    ttk.Label(frame, text="处理流").grid(row=4, column=0, sticky="w", pady=5)
    ttk.Combobox(frame, textvariable=stream_var, values=["BOTH", "BW", "RGB"], width=10, state="readonly").grid(row=4, column=1, sticky="w", pady=5)

    log_box = tk.Text(frame, height=16, wrap="word")
    log_box.grid(row=6, column=0, columnspan=3, sticky="nsew", pady=(10, 0))
    frame.rowconfigure(6, weight=1)

    progress = ttk.Progressbar(frame, mode="indeterminate")
    progress.grid(row=7, column=0, columnspan=3, sticky="ew", pady=(8, 0))

    def choose_dir(var: tk.StringVar) -> None:
        selected = filedialog.askdirectory(initialdir=var.get() or "/mnt/dmx4t/data_all/data")
        if selected:
            var.set(selected)

    def append(message: str) -> None:
        log_box.insert(tk.END, message + "\n")
        log_box.see(tk.END)

    def start() -> None:
        if running["value"]:
            return
        try:
            half_count = int(count_var.get().strip())
        except ValueError:
            messagebox.showerror("参数错误", "每半全景图像帧数必须是整数")
            return
        target_text = output_count_var.get().strip()
        output_count = None
        if target_text:
            try:
                output_count = int(target_text)
            except ValueError:
                messagebox.showerror("参数错误", "要生成全景图数量必须是整数；留空表示全部处理")
                return
            if output_count <= 0:
                messagebox.showerror("参数错误", "要生成全景图数量必须大于 0")
                return
        running["value"] = True
        run_btn.configure(state=tk.DISABLED)
        progress.start(12)
        append("开始处理...")

        def worker() -> None:
            try:
                manifest = block2all_auto(
                    Path(src_var.get()).expanduser(),
                    Path(out_var.get()).expanduser(),
                    half_count,
                    output_count=output_count,
                    stream=stream_var.get(),
                    progress=lambda m: root.after(0, append, m),
                )
                def show_result() -> None:
                    shortage = int(manifest.get("shortageOutputImages", 0) or 0)
                    skipped = int(manifest.get("skippedOutputImages", 0) or 0)
                    failed = int(manifest.get("failedPanoramaGroups", 0) or 0)
                    summary = (
                        f"实际生成: {manifest.get('outputImages')} 张\n"
                        f"跳过已存在: {skipped} 张\n"
                        f"失败全景组: {failed} 个\n"
                    )
                    if shortage > 0:
                        messagebox.showwarning(
                            "数量不足",
                            "源目录图像数量不足。\n"
                            f"目标生成: {manifest.get('requestedOutputImages')} 张\n"
                            f"{summary}"
                            f"不足: {shortage} 张\n\n"
                            f"输出目录:\n{manifest['outDir']}",
                        )
                    else:
                        messagebox.showinfo("完成", f"{summary}\n输出目录:\n{manifest['outDir']}")
                root.after(0, show_result)
            except Exception as exc:  # noqa: BLE001 - GUI should show any failure.
                err = str(exc)
                root.after(0, append, f"错误: {err}")
                root.after(0, lambda: messagebox.showerror("处理失败", err))
            finally:
                def done() -> None:
                    progress.stop()
                    run_btn.configure(state=tk.NORMAL)
                    running["value"] = False
                root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    run_btn = ttk.Button(frame, text="开始合并", command=start)
    run_btn.grid(row=5, column=0, columnspan=3, pady=(8, 0))

    append("输入规则: 选择 /mnt/dmx4t/data_all/data 或其中某一天目录；工具会递归读取 YYYYMMDD/<BW|RGB>/<hhmm>/ 下的原始 JPG。")
    append("输出规则: 保存到 /mnt/dmx4t/data_all/YYYYMMDD/<bw|rgb>/<hh>/，并生成 block2all_data_manifest.json。")
    append("命名规则: 使用每组第一帧文件名追加 -A/-B.jpg，例如 BW_20260608_111151_187-B.jpg")
    append("处理规则: 每帧逆时针旋转90度后水平镜像；每个半全景内部按序号逆序拼接；输出 JPG 质量 95%。")

    root.mainloop()
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    if args.src_dir is None and args.out_root is None and args.half_count is None and args.output_count is None:
        return run_gui()
    if args.src_dir is None or args.out_root is None or args.half_count is None:
        raise SystemExit("用法: block2all 原始分块图像路径 目的路径 合并半全景图像的数目 [需要生成的全景图张数]")

    manifest = block2all_auto(
        Path(args.src_dir),
        Path(args.out_root),
        args.half_count,
        output_count=args.output_count,
        stream=args.stream,
        progress=print,
    )
    print(
        f"OK outDir={manifest['outDir']} "
        f"outputImages={manifest['outputImages']} "
        f"shortageOutputImages={manifest['shortageOutputImages']} "
        f"droppedTailFrames={manifest['droppedTailFrames']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
