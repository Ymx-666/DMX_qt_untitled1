#!/usr/bin/env python3
"""Build an offline DMX replay image tree from locally recorded AB panoramas."""

from __future__ import annotations

import argparse
import json
import os
import re
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Set, Tuple

import cv2


AB_NAME_RE = re.compile(r"_(?P<base>\d+)-(?P<half>[AB])\.jpg$", re.IGNORECASE)
PAYLOAD_RE = re.compile(
    r"^(?P<stream>bw|rgb);(?P<path>/data/raw/(?P<day>\d{8})/"
    r"(?P<path_stream>BW|RGB)/\d{4}/(?:BW|RGB)_\d{8}_\d{6}_"
    r"(?P<index>\d+)\.jpg);$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class HalfJob:
    stream: str
    base_index: int
    half: str
    path: Path


@dataclass(frozen=True)
class JobResult:
    source: Path
    written: int
    skipped: int
    bytes_written: int


def load_logged_paths(
    log_root: Path, expected_day: str
) -> Dict[Tuple[str, int], str]:
    result: Dict[Tuple[str, int], str] = {}
    files = sorted(log_root.rglob("*.jsonl"))
    if not files:
        raise ValueError(f"no JSONL files found below {log_root}")

    for path in files:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, 1):
                try:
                    item = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(
                        f"{path}:{line_number}: invalid JSON: {exc}"
                    ) from exc
                payload = item.get("text")
                match = PAYLOAD_RE.fullmatch(
                    payload if isinstance(payload, str) else ""
                )
                if not match:
                    raise ValueError(
                        f"{path}:{line_number}: unsupported payload: {payload!r}"
                    )
                if match.group("day") != expected_day:
                    raise ValueError(
                        f"{path}:{line_number}: expected {expected_day}, "
                        f"got {match.group('day')}"
                    )
                stream = match.group("stream").upper()
                if stream != match.group("path_stream").upper():
                    raise ValueError(
                        f"{path}:{line_number}: stream/path mismatch"
                    )
                key = (stream, int(match.group("index")))
                if key in result:
                    raise ValueError(f"duplicate logged frame: {key}")
                result[key] = match.group("path")
    return result


def find_half_jobs(recording_root: Path) -> List[HalfJob]:
    jobs: List[HalfJob] = []
    seen: Set[Tuple[str, int, str]] = set()
    for stream_dir in ("bw", "rgb"):
        stream = stream_dir.upper()
        root = recording_root / stream_dir
        for path in sorted(root.rglob("*.jpg")):
            match = AB_NAME_RE.search(path.name)
            if not match:
                continue
            base_index = int(match.group("base"))
            half = match.group("half").upper()
            key = (stream, base_index, half)
            if key in seen:
                raise ValueError(f"duplicate AB half: {key}")
            seen.add(key)
            jobs.append(HalfJob(stream, base_index, half, path))
    if not jobs:
        raise ValueError(f"no AB panorama JPG files found below {recording_root}")
    jobs.sort(key=lambda job: (job.base_index, job.stream, job.half))
    return jobs


def covered_indices(jobs: Iterable[HalfJob]) -> Mapping[str, Set[int]]:
    result: Dict[str, Set[int]] = {"BW": set(), "RGB": set()}
    for job in jobs:
        first = job.base_index + (0 if job.half == "A" else 8)
        result[job.stream].update(range(first, first + 8))
    return result


def contiguous_ranges(indices: Sequence[int]) -> List[Tuple[int, int, int]]:
    if not indices:
        return []
    result: List[Tuple[int, int, int]] = []
    first = previous = indices[0]
    for value in indices[1:]:
        if value != previous + 1:
            result.append((first, previous, previous - first + 1))
            first = value
        previous = value
    result.append((first, previous, previous - first + 1))
    return result


def recover_frame(half_panorama, offset: int):
    """Undo RawRecorder::stitchHalfPanorama for one source frame."""
    if half_panorama is None or half_panorama.ndim != 3:
        raise ValueError("invalid AB panorama")
    height, width = half_panorama.shape[:2]
    if height <= 0 or width != height * 8:
        raise ValueError(f"unexpected AB dimensions: {width}x{height}")
    if offset < 0 or offset >= 8:
        raise ValueError(f"invalid half offset: {offset}")

    # AB stores the eight frames in reverse order after an anti-diagonal
    # reflection. That reflection is its own inverse.
    segment = half_panorama[:, (7 - offset) * height : (8 - offset) * height]
    return cv2.flip(cv2.transpose(segment), -1)


def output_path(output_root: Path, device_path: str) -> Path:
    if not device_path.startswith("/data/"):
        raise ValueError(f"unexpected device path: {device_path}")
    return output_root / device_path[len("/data/") :]


def process_half(
    job: HalfJob,
    selected_indices: Set[int],
    logged_paths: Mapping[Tuple[str, int], str],
    output_root: Path,
    jpeg_quality: int,
) -> JobResult:
    first_index = job.base_index + (0 if job.half == "A" else 8)
    outputs = []
    for offset in range(8):
        index = first_index + offset
        if index not in selected_indices:
            continue
        device_path = logged_paths.get((job.stream, index))
        if device_path:
            outputs.append((offset, output_path(output_root, device_path)))
    if not outputs:
        return JobResult(job.path, 0, 0, 0)

    missing = [item for item in outputs if not item[1].is_file()]
    if not missing:
        return JobResult(job.path, 0, len(outputs), 0)

    panorama = cv2.imread(str(job.path), cv2.IMREAD_COLOR)
    if panorama is None:
        raise ValueError(f"failed to decode {job.path}")

    written = 0
    skipped = 0
    bytes_written = 0
    for offset, target in outputs:
        if target.is_file():
            skipped += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        frame = recover_frame(panorama, offset)
        temporary = target.with_name(target.name + ".part.jpg")
        ok = cv2.imwrite(
            str(temporary), frame, [cv2.IMWRITE_JPEG_QUALITY, jpeg_quality]
        )
        if not ok:
            raise ValueError(f"failed to write {temporary}")
        os.replace(temporary, target)
        written += 1
        bytes_written += target.stat().st_size
    return JobResult(job.path, written, skipped, bytes_written)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--recording-root",
        default="/mnt/dmx4t/data/recordings/20260723",
    )
    parser.add_argument(
        "--log-root",
        default="/mnt/dmx4t/data/raw_log/20260723",
    )
    parser.add_argument(
        "--output-root",
        default="/mnt/dmx4t/data/replay_sources/baseline_20260723",
    )
    parser.add_argument("--expected-day", default="20260723")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--jpeg-quality", type=int, default=100)
    parser.add_argument("--max-halves", type=int, default=0)
    return parser


def main() -> int:
    args = build_argument_parser().parse_args()
    recording_root = Path(args.recording_root)
    log_root = Path(args.log_root)
    output_root = Path(args.output_root)
    workers = max(1, min(args.workers, 12))
    jpeg_quality = max(70, min(args.jpeg_quality, 100))

    cv2.setNumThreads(1)
    logged_paths = load_logged_paths(log_root, args.expected_day)
    jobs = find_half_jobs(recording_root)
    coverage = covered_indices(jobs)
    logged_bw = {index for stream, index in logged_paths if stream == "BW"}
    logged_rgb = {index for stream, index in logged_paths if stream == "RGB"}
    selected = sorted(
        coverage["BW"] & coverage["RGB"] & logged_bw & logged_rgb
    )
    if not selected:
        raise ValueError("no paired RGB/BW frames can be recovered")
    selected_set = set(selected)

    if args.max_halves > 0:
        jobs = jobs[: args.max_halves]
        limited_coverage = covered_indices(jobs)
        selected_set &= limited_coverage["BW"] | limited_coverage["RGB"]

    output_root.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    total_written = total_skipped = total_bytes = completed = 0
    print(
        f"[local-replay] AB halves={len(jobs)} paired_frames={len(selected)} "
        f"workers={workers} output={output_root}",
        flush=True,
    )

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [
            executor.submit(
                process_half,
                job,
                selected_set,
                logged_paths,
                output_root,
                jpeg_quality,
            )
            for job in jobs
        ]
        for future in as_completed(futures):
            result = future.result()
            completed += 1
            total_written += result.written
            total_skipped += result.skipped
            total_bytes += result.bytes_written
            if completed % 20 == 0 or completed == len(futures):
                elapsed = max(0.001, time.monotonic() - started)
                print(
                    f"[local-replay] {completed}/{len(futures)} halves "
                    f"written={total_written} skipped={total_skipped} "
                    f"new_gib={total_bytes / (1024 ** 3):.2f} "
                    f"halves_per_sec={completed / elapsed:.2f}",
                    flush=True,
                )

    if args.max_halves > 0:
        print("[local-replay] limited run complete; manifest not written", flush=True)
        return 0

    missing = []
    for index in selected:
        for stream in ("BW", "RGB"):
            target = output_path(output_root, logged_paths[(stream, index)])
            if not target.is_file():
                missing.append(str(target))
    if missing:
        raise ValueError(
            f"recovery incomplete: {len(missing)} files missing; first={missing[0]}"
        )

    manifest = {
        "status": "complete",
        "day": args.expected_day,
        "sourceRecordingRoot": str(recording_root),
        "sourceLogRoot": str(log_root),
        "outputRoot": str(output_root),
        "pairedFrames": len(selected),
        "outputFiles": len(selected) * 2,
        "firstFrameIndex": selected[0],
        "lastFrameIndex": selected[-1],
        "contiguousRanges": [
            {"first": first, "last": last, "count": count}
            for first, last, count in contiguous_ranges(selected)
        ],
        "jpegQuality": jpeg_quality,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    manifest_path = output_root / "local_replay_manifest.json"
    temporary_manifest = manifest_path.with_suffix(".json.part")
    temporary_manifest.write_text(
        json.dumps(manifest, ensure_ascii=True, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary_manifest, manifest_path)
    print(json.dumps(manifest, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
