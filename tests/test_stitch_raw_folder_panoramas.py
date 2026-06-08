import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import stitch_raw_folder_panoramas as stitch


class StitchRawFolderPanoramasTests(unittest.TestCase):
    def test_smb_uri_candidates_use_gvfs_share_layout(self):
        candidates = stitch.smb_uri_to_gvfs_candidates(
            "smb://tg-ds2309.local/data/raw/20260608/RGB/1153",
            uid=1000,
        )

        self.assertEqual(
            candidates[0],
            Path("/run/user/1000/gvfs/smb-share:server=tg-ds2309.local,share=data/raw/20260608/RGB/1153"),
        )

    def test_default_source_points_to_1153_folder(self):
        self.assertEqual(
            stitch.DEFAULT_SOURCE,
            "smb://tg-ds2309.local/data/raw/20260608/RGB/1153",
        )

    def test_output_label_uses_source_folder_name(self):
        self.assertEqual(stitch.folder_label_from_source(Path("/mnt/raw/RGB/1153")), "1153")
        self.assertEqual(stitch.safe_label("1153 raw"), "1153_raw")

    def test_list_image_files_sorts_by_trailing_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ["RGB_10.jpg", "RGB_2.jpg", "RGB_1.jpg", "note.txt", "RGB_11.jpeg"]:
                (root / name).write_bytes(b"x")

            files = stitch.list_image_files(root)

            self.assertEqual([p.name for p in files], ["RGB_1.jpg", "RGB_2.jpg", "RGB_10.jpg", "RGB_11.jpeg"])

    def test_list_image_files_uses_dmx_numeric_suffix_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in [
                "RGB_20260608_115310_2728.jpg",
                "RGB_20260608_115310_2726.jpg",
                "RGB_20260608_115310_2727.jpg",
                "RGB_20260608_115310_preview.jpg",
                "RGB_20260608_115310_2729.tmp",
                "other_1.jpg",
            ]:
                (root / name).write_bytes(b"x")

            files = stitch.list_image_files(root)

            self.assertEqual(
                [p.name for p in files],
                [
                    "RGB_20260608_115310_2726.jpg",
                    "RGB_20260608_115310_2727.jpg",
                    "RGB_20260608_115310_2728.jpg",
                ],
            )

    def test_group_frames_uses_complete_groups_only(self):
        frames = [Path(f"{i}.jpg") for i in range(34)]

        groups = stitch.group_frames(frames, frames_per_panorama=16)

        self.assertEqual(len(groups), 2)
        self.assertEqual(groups[0], frames[:16])
        self.assertEqual(groups[1], frames[16:32])

    def test_arg_parser_reverses_each_group_by_default(self):
        args = stitch.build_arg_parser().parse_args([])

        self.assertTrue(args.reverse)

    def test_arg_parser_can_keep_forward_order_for_comparison(self):
        args = stitch.build_arg_parser().parse_args(["--forward"])

        self.assertFalse(args.reverse)

    def test_stitch_raw_folder_reverses_each_group_by_default(self):
        with tempfile.TemporaryDirectory() as src_tmp, tempfile.TemporaryDirectory() as out_tmp:
            src = Path(src_tmp)
            out = Path(out_tmp)
            for suffix in range(1, 5):
                (src / f"RGB_20260608_115310_{suffix}.jpg").write_bytes(b"x")

            original_stitch_group = stitch.stitch_group
            original_write_preview = stitch.write_preview
            try:
                stitch.stitch_group = lambda *args, **kwargs: (0, 0, 0)
                stitch.write_preview = lambda *args, **kwargs: None

                manifest = stitch.stitch_raw_folder(
                    str(src),
                    out,
                    frames_per_panorama=4,
                )
            finally:
                stitch.stitch_group = original_stitch_group
                stitch.write_preview = original_write_preview

            self.assertEqual(
                [Path(p).name for p in manifest["panoramas"][0]["frames"]],
                [
                    "RGB_20260608_115310_4.jpg",
                    "RGB_20260608_115310_3.jpg",
                    "RGB_20260608_115310_2.jpg",
                    "RGB_20260608_115310_1.jpg",
                ],
            )


if __name__ == "__main__":
    unittest.main()
