import importlib.util
import sys
from pathlib import Path

import cv2
import numpy as np


MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "build_local_replay_from_ab.py"
)
SPEC = importlib.util.spec_from_file_location("build_local_replay_from_ab", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
LOCAL_REPLAY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LOCAL_REPLAY
SPEC.loader.exec_module(LOCAL_REPLAY)


def orient_frame(frame):
    return cv2.flip(cv2.transpose(frame), -1)


def test_recover_frame_undoes_ab_stitch_order_and_orientation():
    frames = []
    for index in range(8):
        frame = np.zeros((4, 4, 3), dtype=np.uint8)
        frame[:, :, 0] = index * 20
        frame[:, :, 1] = np.arange(16, dtype=np.uint8).reshape(4, 4)
        frames.append(frame)

    panorama = np.hstack([orient_frame(frame) for frame in reversed(frames)])

    for index, expected in enumerate(frames):
        actual = LOCAL_REPLAY.recover_frame(panorama, index)
        assert np.array_equal(actual, expected)


def test_contiguous_ranges_preserves_recording_gaps():
    assert LOCAL_REPLAY.contiguous_ranges([4, 5, 6, 10, 11, 20]) == [
        (4, 6, 3),
        (10, 11, 2),
        (20, 20, 1),
    ]
