import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import stitch_recording_panoramas as stitch


class StitchRecordingPanoramasTests(unittest.TestCase):
    def test_load_index_normalizes_streams_and_relative_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp)
            with (session / "index.jsonl").open("w", encoding="utf-8") as f:
                f.write(json.dumps({"t": 20, "stream": "rgb", "fileIdx": 2, "file": "rgb/2.jpg"}) + "\n")
                f.write(json.dumps({"t": 10, "stream": "GRAY", "fileIdx": 1, "file": "bw/1.jpg"}) + "\n")

            records = stitch.load_index(session)

            self.assertEqual([r.stream for r in records], ["RGB", "BW"])
            self.assertEqual(records[0].path, session / "rgb" / "2.jpg")
            self.assertEqual(records[1].path, session / "bw" / "1.jpg")

    def test_group_complete_panoramas_sorts_and_drops_incomplete_tail(self):
        records = []
        for i in range(17, 0, -1):
            records.append(stitch.FrameRecord("RGB", i * 10, i, Path(f"rgb/{i}.jpg")))
        for i in range(100, 116):
            records.append(stitch.FrameRecord("BW", i * 10, i, Path(f"bw/{i}.jpg")))

        groups = stitch.group_complete_panoramas(records, frames_per_panorama=16)

        self.assertEqual(len(groups["RGB"]), 1)
        self.assertEqual(len(groups["BW"]), 1)
        self.assertEqual([r.file_idx for r in groups["RGB"][0]], list(range(1, 17)))
        self.assertEqual([r.file_idx for r in groups["BW"][0]], list(range(100, 116)))

    def test_group_complete_panoramas_falls_back_to_time_order_without_file_idx(self):
        records = [
            stitch.FrameRecord("RGB", 300, 0, Path("rgb/c.jpg")),
            stitch.FrameRecord("RGB", 100, 0, Path("rgb/a.jpg")),
            stitch.FrameRecord("RGB", 200, 0, Path("rgb/b.jpg")),
        ]

        groups = stitch.group_complete_panoramas(records, frames_per_panorama=2)

        self.assertEqual([[r.path.name for r in g] for g in groups["RGB"]], [["a.jpg", "b.jpg"]])

    def test_orient_frame_can_rotate_without_horizontal_mirror(self):
        import cv2
        import numpy as np

        img = np.array([[1, 2, 3], [4, 5, 6]], dtype=np.uint8)

        rotated = stitch._orient_frame(cv2, img, orient=True, mirror=False)
        mirrored = stitch._orient_frame(cv2, img, orient=True, mirror=True)

        self.assertEqual(rotated.tolist(), [[3, 6], [2, 5], [1, 4]])
        self.assertEqual(mirrored.tolist(), [[6, 3], [5, 2], [4, 1]])


if __name__ == "__main__":
    unittest.main()
