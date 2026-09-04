import importlib.util
import json
import sys
from pathlib import Path

import pytest


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "replay_raw_udp_20260723.py"
SPEC = importlib.util.spec_from_file_location("replay_raw_udp_20260723", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
REPLAY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPLAY
SPEC.loader.exec_module(REPLAY)


def write_record(handle, rx_ms, stream, frame_index):
    stream_upper = stream.upper()
    text = (
        f"{stream.lower()};/data/raw/20260723/{stream_upper}/1200/"
        f"{stream_upper}_20260723_120000_{frame_index}.jpg;"
    )
    handle.write(json.dumps({"rxMs": str(rx_ms), "text": text}) + "\n")


def test_load_dataset_pairs_split_files_and_sorts_by_timestamp(tmp_path):
    first = tmp_path / "15" / "part1.jsonl"
    second = tmp_path / "16" / "part2.jsonl"
    first.parent.mkdir()
    second.parent.mkdir()
    with first.open("w", encoding="utf-8") as handle:
        write_record(handle, 1000, "BW", 0)
        write_record(handle, 1102, "RGB", 0)
        write_record(handle, 1602, "RGB", 1)
    with second.open("w", encoding="utf-8") as handle:
        write_record(handle, 1500, "BW", 1)

    dataset = REPLAY.load_dataset(tmp_path)

    assert len(dataset.records) == 4
    assert [pair.frame_index for pair in dataset.pairs] == [0, 1]
    assert dataset.pair_offset_median_ms == 102.0
    assert [record.rx_ms for record in dataset.records] == [1000, 1102, 1500, 1602]


def test_build_events_uses_fixed_eight_second_revolution():
    bw = REPLAY.RawRecord(1000, "bw;/data/a;", "BW", "/data/a", 0, "a", 1)
    rgb = REPLAY.RawRecord(1102, "rgb;/data/b;", "RGB", "/data/b", 0, "a", 2)
    pairs = [
        REPLAY.FramePair(0, bw, rgb),
        REPLAY.FramePair(1, bw, rgb),
    ]

    events, pass_seconds = REPLAY.build_events(
        pairs,
        revolution_seconds=8.0,
        segments=16,
        pair_offset_ms=102.0,
    )

    assert [event.offset_s for event in events] == [0.0, 0.102, 0.5, 0.602]
    assert pass_seconds == 1.0


def test_load_dataset_rejects_incomplete_pair(tmp_path):
    path = tmp_path / "part.jsonl"
    with path.open("w", encoding="utf-8") as handle:
        write_record(handle, 1000, "BW", 0)

    with pytest.raises(ValueError, match="incomplete"):
        REPLAY.load_dataset(tmp_path)


def test_build_events_rejects_pair_offset_outside_frame_period():
    bw = REPLAY.RawRecord(1000, "bw;/data/a;", "BW", "/data/a", 0, "a", 1)
    rgb = REPLAY.RawRecord(1102, "rgb;/data/b;", "RGB", "/data/b", 0, "a", 2)

    with pytest.raises(ValueError, match="pair offset"):
        REPLAY.build_events(
            [REPLAY.FramePair(0, bw, rgb)],
            revolution_seconds=8.0,
            segments=16,
            pair_offset_ms=500.0,
        )


def test_available_pairs_skips_missing_local_images(tmp_path):
    def record(stream, index):
        path = f"/data/raw/20260723/{stream}/1200/{stream}_{index}.jpg"
        return REPLAY.RawRecord(1000, "", stream, path, index, "a", 1)

    pairs = [
        REPLAY.FramePair(0, record("BW", 0), record("RGB", 0)),
        REPLAY.FramePair(1, record("BW", 1), record("RGB", 1)),
    ]
    for stream in ("BW", "RGB"):
        path = tmp_path / "raw" / "20260723" / stream / "1200" / f"{stream}_0.jpg"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"image")

    available, skipped = REPLAY.available_pairs(pairs, tmp_path)

    assert [pair.frame_index for pair in available] == [0]
    assert skipped == 1
