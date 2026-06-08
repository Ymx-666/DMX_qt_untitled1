import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import stitch_raw_tree_panoramas as batch


class StitchRawTreePanoramasTests(unittest.TestCase):
    def test_discover_raw_folders_finds_rgb_and_bw_minute_dirs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "20260608"
            (root / "RGB" / "1036").mkdir(parents=True)
            (root / "RGB" / "1035").mkdir(parents=True)
            (root / "BW" / "1103").mkdir(parents=True)
            (root / "RGB" / "note.txt").write_text("ignore", encoding="utf-8")

            jobs = batch.discover_raw_folders(root)

            self.assertEqual(
                [(j.stream, j.date_label, j.folder_label) for j in jobs],
                [
                    ("RGB", "20260608", "1035"),
                    ("RGB", "20260608", "1036"),
                    ("BW", "20260608", "1103"),
                ],
            )

    def test_batch_stitch_tree_names_outputs_and_reverses_groups(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            root = Path(src_tmp) / "20260608"
            src = root / "RGB" / "1035"
            src.mkdir(parents=True)
            for suffix in range(1, 5):
                (src / f"RGB_20260608_103500_{suffix}.jpg").write_bytes(b"x")

            calls = []
            original_stitch_group = batch.stitch_group
            original_write_preview = batch.write_preview
            try:
                def fake_stitch_group(records, out_path, **kwargs):
                    calls.append((records, out_path, kwargs))
                    return (16384, 4096, 1)

                batch.stitch_group = fake_stitch_group
                batch.write_preview = lambda *args, **kwargs: None

                manifest = batch.batch_stitch_tree(
                    root,
                    Path(out_tmp),
                    frames_per_panorama=4,
                    show_progress=False,
                )
            finally:
                batch.stitch_group = original_stitch_group
                batch.write_preview = original_write_preview

            self.assertEqual(manifest["totalPanoramas"], 1)
            pano = manifest["folders"][0]["panoramas"][0]
            self.assertEqual(pano["file"], "rgb_20260608_1035_pano_0001.tiff")
            self.assertEqual(pano["preview"], "rgb_20260608_1035_pano_0001_preview.jpg")
            self.assertEqual(
                [r.path.name for r in calls[0][0]],
                [
                    "RGB_20260608_103500_4.jpg",
                    "RGB_20260608_103500_3.jpg",
                    "RGB_20260608_103500_2.jpg",
                    "RGB_20260608_103500_1.jpg",
                ],
            )

    def test_render_progress_line_includes_folder_and_counts(self):
        line = batch.render_progress_line(
            done=2,
            total=10,
            current="BW/1130",
            panorama_count=3,
            dropped_tail_frames=5,
        )

        self.assertIn("2/10 folders", line)
        self.assertIn("BW/1130", line)
        self.assertIn("3 panoramas", line)
        self.assertIn("dropped 5", line)

    def test_batch_stitch_tree_can_write_progress(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            root = Path(src_tmp) / "20260608"
            src = root / "BW" / "1130"
            src.mkdir(parents=True)
            for suffix in range(1, 5):
                (src / f"BW_20260608_113000_{suffix}.jpg").write_bytes(b"x")

            progress = io.StringIO()
            original_stitch_group = batch.stitch_group
            original_write_preview = batch.write_preview
            try:
                batch.stitch_group = lambda *args, **kwargs: (16384, 4096, 1)
                batch.write_preview = lambda *args, **kwargs: None

                batch.batch_stitch_tree(
                    root,
                    Path(out_tmp),
                    frames_per_panorama=4,
                    show_progress=True,
                    progress_stream=progress,
                )
            finally:
                batch.stitch_group = original_stitch_group
                batch.write_preview = original_write_preview

            self.assertIn("BW/1130", progress.getvalue())
            self.assertTrue(progress.getvalue().endswith("\n"))


if __name__ == "__main__":
    unittest.main()
