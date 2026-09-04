import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import block2all


class Block2AllTests(unittest.TestCase):
    def test_trailing_index_and_sort(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in [
                "BW_20260616_134920_1015.jpg",
                "BW_20260616_134920_1013.jpg",
                "BW_20260616_134920_1014.jpg",
                "BW_20260616_134920_preview.jpg",
                "note.txt",
            ]:
                (root / name).write_bytes(b"x")

            files = block2all.list_block_images(root)

            self.assertEqual([f.path.name for f in files], [
                "BW_20260616_134920_1013.jpg",
                "BW_20260616_134920_1014.jpg",
                "BW_20260616_134920_1015.jpg",
            ])

    def test_output_session_name_from_first_file(self):
        self.assertEqual(
            block2all.infer_session_name(Path("BW_20260616_134920_1013.jpg")),
            "ALL2_20260616_134920",
        )

    def test_block2all_names_a_b_and_reverses_each_half(self):
        try:
            import cv2  # noqa: F401
            import numpy as np
        except ImportError:
            self.skipTest("OpenCV/numpy not installed")

        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp)
            out = Path(out_tmp)
            for idx in range(1013, 1029):
                # Tiny image is enough to verify grouping/output naming.
                img = np.full((2, 3, 3), idx % 255, dtype=np.uint8)
                self.assertTrue(cv2.imwrite(str(src / f"BW_20260616_134920_{idx}.jpg"), img))

            manifest = block2all.block2all(src, out, 8)

            out_dir = Path(manifest["outDir"])
            self.assertTrue((out_dir / "BW_20260616_134920_1013-A.jpg").is_file())
            self.assertTrue((out_dir / "BW_20260616_134920_1013-B.jpg").is_file())
            self.assertTrue((out_dir / "manifest.json").is_file())

            data = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(data["outputImages"], 2)
            self.assertEqual(
                [Path(p).name for p in data["outputs"][0]["frames"]],
                [f"BW_20260616_134920_{idx}.jpg" for idx in range(1020, 1012, -1)],
            )

    def test_block2all_limits_requested_output_count(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp)
            out = Path(out_tmp)
            for idx in range(1013, 1029):
                (src / f"BW_20260616_134920_{idx}.jpg").write_bytes(b"x")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            try:
                block2all.stitch_half = lambda paths: FakeImage()
                block2all.write_jpg = lambda path, image: path.write_bytes(b"jpg")

                manifest = block2all.block2all(src, out, 8, output_count=1)
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            self.assertEqual(manifest["availableOutputImages"], 2)
            self.assertEqual(manifest["outputImages"], 1)
            self.assertEqual(manifest["unprocessedFramesByLimit"], 8)

    def test_block2all_reports_shortage_when_requested_count_is_too_large(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp)
            out = Path(out_tmp)
            for idx in range(1013, 1029):
                (src / f"BW_20260616_134920_{idx}.jpg").write_bytes(b"x")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            try:
                block2all.stitch_half = lambda paths: FakeImage()
                block2all.write_jpg = lambda path, image: path.write_bytes(b"jpg")

                manifest = block2all.block2all(src, out, 8, output_count=3)
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            self.assertEqual(manifest["availableOutputImages"], 2)
            self.assertEqual(manifest["outputImages"], 2)
            self.assertEqual(manifest["shortageOutputImages"], 1)

    def test_recording_mode_reads_index_and_outputs_by_session_stream(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            root = Path(src_tmp)
            session = root / "REC2_20260617_120000"
            bw = session / "bw"
            rgb = session / "rgb"
            bw.mkdir(parents=True)
            rgb.mkdir()
            with (session / "index.jsonl").open("w", encoding="utf-8") as f:
                for idx in range(1, 10):
                    name = f"{idx}_100{idx}.jpg"
                    (bw / name).write_bytes(b"x")
                    f.write(json.dumps({"t": 1000 + idx, "stream": "BW", "fileIdx": idx, "file": f"bw/{name}"}) + "\n")
                for idx in range(1, 5):
                    name = f"{idx}_200{idx}.jpg"
                    (rgb / name).write_bytes(b"x")
                    f.write(json.dumps({"t": 2000 + idx, "stream": "RGB", "fileIdx": idx, "file": f"rgb/{name}"}) + "\n")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            try:
                block2all.stitch_half = lambda paths: FakeImage()
                block2all.write_jpg = lambda path, image: path.write_bytes(b"jpg")

                manifest = block2all.block2all_recordings(root, Path(out_tmp), 8, stream="BW")
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            self.assertEqual(manifest["mode"], "recordings")
            self.assertEqual(manifest["availableOutputImages"], 1)
            self.assertEqual(manifest["outputImages"], 1)
            out_dir = Path(out_tmp) / "20260617" / "ALL2_20260617_120000" / "bw"
            outputs = list(out_dir.glob("*.jpg"))
            self.assertEqual(len(outputs), 1)
            self.assertTrue(outputs[0].name.startswith("BW_1_1001_000001-A"))
            self.assertEqual(
                manifest["sessions"][0]["allDir"],
                str(Path(out_tmp) / "20260617" / "ALL2_20260617_120000"),
            )
            self.assertTrue((Path(out_tmp) / "block2all_recordings_manifest.json").is_file())

    def test_recording_mode_applies_global_output_limit_across_sessions(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            root = Path(src_tmp)
            for sess_idx in range(2):
                session = root / f"REC2_20260617_12000{sess_idx}"
                bw = session / "bw"
                bw.mkdir(parents=True)
                with (session / "index.jsonl").open("w", encoding="utf-8") as f:
                    for idx in range(1, 17):
                        name = f"{idx}_100{idx}.jpg"
                        (bw / name).write_bytes(b"x")
                        f.write(json.dumps({"t": 1000 + idx, "stream": "BW", "fileIdx": idx, "file": f"bw/{name}"}) + "\n")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            try:
                block2all.stitch_half = lambda paths: FakeImage()
                block2all.write_jpg = lambda path, image: path.write_bytes(b"jpg")

                manifest = block2all.block2all_recordings(root, Path(out_tmp), 8, output_count=3, stream="BW")
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            self.assertEqual(manifest["availableOutputImages"], 4)
            self.assertEqual(manifest["outputImages"], 3)
            self.assertEqual(manifest["unprocessedFramesByLimit"], 8)

    def test_data_tree_mode_outputs_day_stream_hour_dirs(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp) / "data"
            bw = src / "20260608" / "BW" / "1115"
            bw.mkdir(parents=True)
            for offset, idx in enumerate(range(187, 203)):
                second = 51 + offset // 2
                (bw / f"BW_20260608_1111{second:02d}_{idx}.jpg").write_bytes(b"x")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            def fake_write_jpg(path, image):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"jpg")

            try:
                block2all.stitch_half = lambda paths: FakeImage()
                block2all.write_jpg = fake_write_jpg

                manifest = block2all.block2all_data_tree(src, Path(out_tmp), 8, stream="BW")
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            self.assertEqual(manifest["mode"], "data_tree")
            self.assertEqual(manifest["availableOutputImages"], 2)
            self.assertEqual(manifest["outputImages"], 2)
            out_dir = Path(out_tmp) / "20260608" / "bw" / "11"
            self.assertTrue((out_dir / "BW_20260608_111151_187-A.jpg").is_file())
            self.assertTrue((out_dir / "BW_20260608_111151_187-B.jpg").is_file())
            self.assertTrue((Path(out_tmp) / "block2all_data_manifest.json").is_file())


    def test_data_tree_mode_skips_existing_outputs(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp) / "data"
            bw = src / "20260608" / "BW" / "1115"
            bw.mkdir(parents=True)
            for offset, idx in enumerate(range(187, 203)):
                second = 51 + offset // 2
                (bw / f"BW_20260608_1111{second:02d}_{idx}.jpg").write_bytes(b"x")

            existing_dir = Path(out_tmp) / "20260608" / "bw" / "11"
            existing_dir.mkdir(parents=True)
            (existing_dir / "BW_20260608_111151_187-A.jpg").write_bytes(b"old-a")
            (existing_dir / "BW_20260608_111151_187-B.jpg").write_bytes(b"old-b")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            writes = []

            def fake_write_jpg(path, image):
                writes.append(path.name)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"jpg")

            try:
                block2all.stitch_half = lambda paths: FakeImage()
                block2all.write_jpg = fake_write_jpg

                manifest = block2all.block2all_data_tree(src, Path(out_tmp), 8, stream="BW")
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            self.assertEqual(manifest["outputImages"], 0)
            self.assertEqual(manifest["skippedOutputImages"], 2)
            self.assertEqual(manifest["failedPanoramaGroups"], 0)
            self.assertEqual(writes, [])
            self.assertEqual((existing_dir / "BW_20260608_111151_187-A.jpg").read_bytes(), b"old-a")

    def test_data_tree_mode_read_failure_skips_whole_ab_group_and_continues(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp) / "data"
            bw = src / "20260608" / "BW" / "1115"
            bw.mkdir(parents=True)
            for offset, idx in enumerate(range(187, 219)):
                second = 51 + offset // 2
                (bw / f"BW_20260608_1111{second:02d}_{idx}.jpg").write_bytes(b"x")

            original_stitch_half = block2all.stitch_half
            original_write_jpg = block2all.write_jpg

            class FakeImage:
                shape = (2, 24, 3)

            calls = {"count": 0}

            def fake_stitch_half(paths):
                calls["count"] += 1
                if calls["count"] == 2:
                    raise RuntimeError("读取失败: bad.jpg")
                return FakeImage()

            def fake_write_jpg(path, image):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"jpg")

            try:
                block2all.stitch_half = fake_stitch_half
                block2all.write_jpg = fake_write_jpg

                manifest = block2all.block2all_data_tree(src, Path(out_tmp), 8, stream="BW")
            finally:
                block2all.stitch_half = original_stitch_half
                block2all.write_jpg = original_write_jpg

            out_dir = Path(out_tmp) / "20260608" / "bw" / "11"
            self.assertFalse((out_dir / "BW_20260608_111151_187-A.jpg").exists())
            self.assertFalse((out_dir / "BW_20260608_111151_187-B.jpg").exists())
            self.assertTrue((out_dir / "BW_20260608_111159_203-A.jpg").is_file())
            self.assertTrue((out_dir / "BW_20260608_111159_203-B.jpg").is_file())
            self.assertEqual(manifest["outputImages"], 2)
            self.assertEqual(manifest["skippedOutputImages"], 0)
            self.assertEqual(manifest["failedPanoramaGroups"], 1)
            self.assertIn("读取失败", manifest["streams"][0]["failed"][0]["error"])



if __name__ == "__main__":
    unittest.main()
