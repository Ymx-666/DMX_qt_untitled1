#!/usr/bin/env python3
"""Replay the 2026-07-23 DMX path log on an isolated local UDP port."""

from __future__ import annotations

import argparse
import json
import re
import select
import signal
import socket
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


PAYLOAD_RE = re.compile(
    r"^(?P<type>bw|rgb);"
    r"(?P<path>/data/raw/(?P<day>\d{8})/(?P<stream>BW|RGB)/\d{4}/"
    r"(?:BW|RGB)_\d{8}_\d{6}_(?P<index>\d+)\.jpg);$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class RawRecord:
    rx_ms: int
    text: str
    stream: str
    device_path: str
    frame_index: int
    source_file: str
    source_line: int


@dataclass(frozen=True)
class FramePair:
    frame_index: int
    bw: RawRecord
    rgb: RawRecord


@dataclass(frozen=True)
class ReplayEvent:
    offset_s: float
    text: str
    frame_index: int
    stream: str


@dataclass(frozen=True)
class Dataset:
    records: List[RawRecord]
    pairs: List[FramePair]
    pair_offset_median_ms: float
    first_rx_ms: int
    last_rx_ms: int
    long_gaps: List[int]


def _percentile(values: Iterable[int], ratio: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    pos = (len(ordered) - 1) * ratio
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    fraction = pos - lo
    return ordered[lo] * (1.0 - fraction) + ordered[hi] * fraction


def load_dataset(log_root: Path, expected_day: str = "20260723") -> Dataset:
    files = sorted(log_root.rglob("*.jsonl"))
    if not files:
        raise ValueError(f"no JSONL files found below {log_root}")

    records: List[RawRecord] = []
    for path in files:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, 1):
                try:
                    item = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc

                text = item.get("text")
                match = PAYLOAD_RE.fullmatch(text if isinstance(text, str) else "")
                if not match:
                    raise ValueError(f"{path}:{line_number}: unsupported payload: {text!r}")
                stream = match.group("type").upper()
                path_stream = match.group("stream").upper()
                if stream != path_stream:
                    raise ValueError(
                        f"{path}:{line_number}: payload stream {stream} does not match path {path_stream}"
                    )
                if match.group("day") != expected_day:
                    raise ValueError(
                        f"{path}:{line_number}: expected day {expected_day}, got {match.group('day')}"
                    )
                try:
                    rx_ms = int(item["rxMs"])
                except (KeyError, TypeError, ValueError) as exc:
                    raise ValueError(f"{path}:{line_number}: invalid rxMs") from exc
                records.append(
                    RawRecord(
                        rx_ms=rx_ms,
                        text=text,
                        stream=stream,
                        device_path=match.group("path"),
                        frame_index=int(match.group("index")),
                        source_file=str(path),
                        source_line=line_number,
                    )
                )

    records.sort(key=lambda item: (item.rx_ms, item.source_file, item.source_line))
    by_index: Dict[int, Dict[str, RawRecord]] = {}
    for record in records:
        streams = by_index.setdefault(record.frame_index, {})
        if record.stream in streams:
            previous = streams[record.stream]
            raise ValueError(
                f"duplicate {record.stream} frame {record.frame_index}: "
                f"{previous.source_file}:{previous.source_line} and "
                f"{record.source_file}:{record.source_line}"
            )
        streams[record.stream] = record

    indices = sorted(by_index)
    expected_indices = list(range(indices[0], indices[-1] + 1))
    if indices != expected_indices:
        missing = sorted(set(expected_indices) - set(indices))
        raise ValueError(f"non-contiguous frame indices; first missing values: {missing[:10]}")

    pairs: List[FramePair] = []
    for frame_index in indices:
        streams = by_index[frame_index]
        if set(streams) != {"BW", "RGB"}:
            raise ValueError(f"frame {frame_index} is incomplete: {sorted(streams)}")
        pairs.append(FramePair(frame_index, streams["BW"], streams["RGB"]))

    healthy_pair_offsets = [
        pair.rgb.rx_ms - pair.bw.rx_ms
        for pair in pairs
        if 0 <= pair.rgb.rx_ms - pair.bw.rx_ms < 500
    ]
    if not healthy_pair_offsets:
        raise ValueError("no valid BW/RGB pair offsets found")

    intervals = [
        records[index].rx_ms - records[index - 1].rx_ms
        for index in range(1, len(records))
    ]
    return Dataset(
        records=records,
        pairs=pairs,
        pair_offset_median_ms=float(statistics.median(healthy_pair_offsets)),
        first_rx_ms=records[0].rx_ms,
        last_rx_ms=records[-1].rx_ms,
        long_gaps=[value for value in intervals if value > 2000],
    )


def check_image_paths(dataset: Dataset, share_mount: Path) -> List[Path]:
    missing: List[Path] = []
    for record in dataset.records:
        relative = record.device_path[len("/data/") :]
        local_path = share_mount / relative
        if not local_path.is_file():
            missing.append(local_path)
    return missing


def available_pairs(
    pairs: Iterable[FramePair], share_mount: Path
) -> Tuple[List[FramePair], int]:
    available: List[FramePair] = []
    skipped = 0
    for pair in pairs:
        paths = []
        for record in (pair.bw, pair.rgb):
            relative = record.device_path[len("/data/") :]
            paths.append(share_mount / relative)
        if all(path.is_file() for path in paths):
            available.append(pair)
        else:
            skipped += 1
    return available, skipped


def build_events(
    pairs: List[FramePair],
    revolution_seconds: float,
    segments: int,
    pair_offset_ms: float,
) -> Tuple[List[ReplayEvent], float]:
    if revolution_seconds <= 0.0:
        raise ValueError("revolution seconds must be positive")
    if segments <= 0:
        raise ValueError("segments must be positive")

    pair_period_s = revolution_seconds / float(segments)
    pair_offset_s = pair_offset_ms / 1000.0
    if pair_offset_s < 0.0 or pair_offset_s >= pair_period_s:
        raise ValueError(
            f"pair offset {pair_offset_ms}ms must be in [0, {pair_period_s * 1000.0:.3f})"
        )

    events: List[ReplayEvent] = []
    for pair_number, pair in enumerate(pairs):
        base_offset = pair_number * pair_period_s
        events.append(ReplayEvent(base_offset, pair.bw.text, pair.frame_index, "BW"))
        events.append(
            ReplayEvent(base_offset + pair_offset_s, pair.rgb.text, pair.frame_index, "RGB")
        )
    return events, len(pairs) * pair_period_s


def analysis_dict(
    dataset: Dataset,
    revolution_seconds: float,
    segments: int,
    pair_offset_ms: float,
) -> Dict[str, object]:
    intervals = [
        dataset.records[index].rx_ms - dataset.records[index - 1].rx_ms
        for index in range(1, len(dataset.records))
    ]
    active = [value for value in intervals if value <= 2000]
    frame_period_s = revolution_seconds / float(segments)
    pass_seconds = len(dataset.pairs) * frame_period_s
    return {
        "records": len(dataset.records),
        "bwFrames": len(dataset.pairs),
        "rgbFrames": len(dataset.pairs),
        "frameIndexFirst": dataset.pairs[0].frame_index,
        "frameIndexLast": dataset.pairs[-1].frame_index,
        "sourceWallSpanSeconds": (dataset.last_rx_ms - dataset.first_rx_ms) / 1000.0,
        "sourceActiveDatagramIntervalMs": {
            "mean": statistics.mean(active),
            "p50": _percentile(active, 0.50),
            "p95": _percentile(active, 0.95),
            "p99": _percentile(active, 0.99),
        },
        "sourceLongGapsOver2s": dataset.long_gaps,
        "sourcePairOffsetMedianMs": dataset.pair_offset_median_ms,
        "replayRevolutionSeconds": revolution_seconds,
        "replaySegmentsPerRevolution": segments,
        "replayFramePairPeriodMs": frame_period_s * 1000.0,
        "replayPairOffsetMs": pair_offset_ms,
        "replayDatagramsPerSecond": 2.0 / frame_period_s,
        "replayPassSeconds": pass_seconds,
    }


class ReplayServer:
    def __init__(
        self,
        events: List[ReplayEvent],
        pass_seconds: float,
        dst_ip: str,
        dst_port: int,
        control_ip: str,
        control_port: int,
        reply_ip: str,
        reply_port: int,
        loop: bool,
        progress_seconds: float,
    ) -> None:
        self._events = events
        self._pass_seconds = pass_seconds
        self._dst = (dst_ip, dst_port)
        self._reply = (reply_ip, reply_port)
        self._loop = loop
        self._progress_seconds = progress_seconds
        self._data_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._control_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._control_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._control_socket.bind((control_ip, control_port))
        self._running = True
        self._playing = False
        self._completed = False
        self._event_index = 0
        self._pass_number = 0
        self._anchor_monotonic = 0.0
        self._virtual_elapsed = 0.0
        self._sent = 0
        self._last_progress = 0.0

    def _reply_status(self, state: str) -> None:
        message = (
            f"DMX_REPLAY_{state};event={self._event_index};"
            f"total={len(self._events)};pass={self._pass_number + 1};"
        )
        try:
            self._data_socket.sendto(message.encode("ascii"), self._reply)
        except OSError:
            pass

    def _start(self) -> None:
        now = time.monotonic()
        if self._completed:
            self._event_index = 0
            self._pass_number = 0
            self._virtual_elapsed = 0.0
            self._completed = False
        if not self._playing:
            self._anchor_monotonic = now - self._virtual_elapsed
            self._playing = True
        self._reply_status("STARTED")
        print(
            f"[replay] started event={self._event_index}/{len(self._events)} "
            f"pass={self._pass_number + 1}",
            flush=True,
        )

    def _stop(self) -> None:
        if self._playing:
            self._virtual_elapsed = max(0.0, time.monotonic() - self._anchor_monotonic)
            self._playing = False
        self._reply_status("PAUSED")
        print(
            f"[replay] paused event={self._event_index}/{len(self._events)}",
            flush=True,
        )

    def _restart(self) -> None:
        self._event_index = 0
        self._pass_number = 0
        self._virtual_elapsed = 0.0
        self._completed = False
        self._anchor_monotonic = time.monotonic()
        self._playing = True
        self._reply_status("RESTARTED")
        print("[replay] restarted from frame 0", flush=True)

    def _handle_command(self, payload: bytes) -> None:
        command = payload.decode("utf-8", errors="ignore").strip().upper()
        if command in {"DMX_REPLAY_START;", "START", "START;"}:
            self._start()
        elif command in {"DMX_REPLAY_STOP;", "STOP", "STOP;"}:
            self._stop()
        elif command in {"DMX_REPLAY_RESTART;", "RESTART", "RESTART;"}:
            self._restart()
        elif command in {"DMX_REPLAY_STATUS;", "STATUS", "STATUS;"}:
            self._reply_status("PLAYING" if self._playing else "PAUSED")
        elif command in {"DMX_REPLAY_QUIT;", "QUIT", "QUIT;"}:
            self._running = False

    def _poll_control(self, timeout_s: Optional[float]) -> None:
        readable, _, _ = select.select([self._control_socket], [], [], timeout_s)
        if not readable:
            return
        payload, _ = self._control_socket.recvfrom(4096)
        self._handle_command(payload)

    def _current_event_offset(self) -> float:
        return self._pass_number * self._pass_seconds + self._events[self._event_index].offset_s

    def _advance_event(self) -> None:
        self._event_index += 1
        if self._event_index < len(self._events):
            return
        if self._loop:
            self._event_index = 0
            self._pass_number += 1
            print(f"[replay] completed pass {self._pass_number}", flush=True)
            return
        self._event_index = 0
        self._pass_number = 0
        self._virtual_elapsed = 0.0
        self._playing = False
        self._completed = True
        self._reply_status("COMPLETED")
        print("[replay] completed one pass; waiting for Device Run to replay again", flush=True)

    def run(self, autostart: bool) -> None:
        print(
            f"[replay] control={self._control_socket.getsockname()} "
            f"destination={self._dst} events={len(self._events)} loop={int(self._loop)}",
            flush=True,
        )
        self._reply_status("READY")
        if autostart:
            self._start()

        try:
            while self._running:
                if not self._playing:
                    self._poll_control(1.0)
                    continue

                event = self._events[self._event_index]
                target = self._anchor_monotonic + self._current_event_offset()
                now = time.monotonic()
                wait_s = target - now
                if wait_s > 0.0:
                    self._poll_control(min(wait_s, 0.1))
                    continue

                self._data_socket.sendto(event.text.encode("utf-8"), self._dst)
                self._sent += 1
                now = time.monotonic()
                if (
                    self._progress_seconds > 0.0
                    and now - self._last_progress >= self._progress_seconds
                ):
                    self._last_progress = now
                    print(
                        f"[replay] sent={self._sent} frame={event.frame_index} "
                        f"stream={event.stream} pass={self._pass_number + 1}",
                        flush=True,
                    )
                self._advance_event()
        finally:
            self._control_socket.close()
            self._data_socket.close()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--log-root",
        default="/mnt/dmx4t/data/raw_log/20260723",
        help="directory containing the source JSONL files",
    )
    parser.add_argument("--expected-day", default="20260723")
    parser.add_argument("--share-mount", default="/mnt/dmx_share")
    parser.add_argument("--check-images", action="store_true")
    parser.add_argument(
        "--available-only",
        action="store_true",
        help="replay only RGB/BW pairs present below share-mount",
    )
    parser.add_argument("--analyze", action="store_true")
    parser.add_argument("--dst-ip", default="127.0.0.1")
    parser.add_argument("--dst-port", type=int, default=18001)
    parser.add_argument("--control-ip", default="127.0.0.1")
    parser.add_argument("--control-port", type=int, default=18501)
    parser.add_argument("--reply-ip", default="127.0.0.1")
    parser.add_argument("--reply-port", type=int, default=15002)
    parser.add_argument("--revolution-seconds", type=float, default=8.0)
    parser.add_argument("--segments", type=int, default=16)
    parser.add_argument(
        "--pair-offset-ms",
        type=float,
        default=0.0,
        help="BW-to-RGB delay; zero uses the healthy source median",
    )
    parser.add_argument("--max-pairs", type=int, default=0)
    parser.add_argument("--loop", action="store_true")
    parser.add_argument("--autostart", action="store_true")
    parser.add_argument("--progress-seconds", type=float, default=10.0)
    return parser


def main() -> int:
    args = build_argument_parser().parse_args()
    dataset = load_dataset(Path(args.log_root), args.expected_day)
    pair_offset_ms = (
        args.pair_offset_ms if args.pair_offset_ms > 0.0 else dataset.pair_offset_median_ms
    )

    pairs = dataset.pairs
    skipped_pairs = 0
    if args.available_only:
        pairs, skipped_pairs = available_pairs(pairs, Path(args.share_mount))
        if not pairs:
            print(
                f"no paired images found below {args.share_mount}",
                file=sys.stderr,
            )
            return 2
        print(
            f"[replay] local availability: selected={len(pairs)} "
            f"skipped={skipped_pairs}",
            flush=True,
        )
    elif args.check_images:
        missing = check_image_paths(dataset, Path(args.share_mount))
        if missing:
            for path in missing[:20]:
                print(f"missing image: {path}", file=sys.stderr)
            print(f"missing image count: {len(missing)}", file=sys.stderr)
            return 2

    analysis = analysis_dict(
        dataset,
        args.revolution_seconds,
        args.segments,
        pair_offset_ms,
    )
    analysis["selectedFramePairs"] = len(pairs)
    analysis["skippedUnavailablePairs"] = skipped_pairs
    analysis["replayPassSeconds"] = (
        len(pairs) * args.revolution_seconds / float(args.segments)
    )
    if args.analyze:
        print(json.dumps(analysis, indent=2, sort_keys=True))
        return 0

    if args.max_pairs > 0:
        pairs = pairs[: args.max_pairs]
    events, pass_seconds = build_events(
        pairs,
        args.revolution_seconds,
        args.segments,
        pair_offset_ms,
    )
    print(json.dumps(analysis, sort_keys=True), flush=True)

    server = ReplayServer(
        events=events,
        pass_seconds=pass_seconds,
        dst_ip=args.dst_ip,
        dst_port=args.dst_port,
        control_ip=args.control_ip,
        control_port=args.control_port,
        reply_ip=args.reply_ip,
        reply_port=args.reply_port,
        loop=args.loop,
        progress_seconds=args.progress_seconds,
    )

    def stop_server(_signum: int, _frame: object) -> None:
        server._running = False

    signal.signal(signal.SIGINT, stop_server)
    signal.signal(signal.SIGTERM, stop_server)
    server.run(args.autostart)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
