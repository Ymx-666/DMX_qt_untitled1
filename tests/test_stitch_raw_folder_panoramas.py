import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import stitch_raw_folder_panoramas as stitch


class StitchRawFolderPanoramasTests(unittest.TestCase):
    def test_smb_uri_candidates_use_gvfs_share_layout(self):
        candidates = stitch.smb_uri_to_gvfs_candidates(
            "smb://tg-ds2309.local/data/raw/20260608/RGB/115",
            uid=1000,
        )

        self.assertEqual(
            candidates[0],
            Path("/run/user/1000/gvfs/smb-share:server=tg-ds2309.local,share=data/raw/20260608/RGB/115"),
        )

    def test_list_image_files_sorts_by_trailing_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ["RGB_10.jpg", "RGB_2.jpg", "RGB_1.jpg", "note.txt", "RGB_11.jpeg"]:
                (root / name).write_bytes(b"x")

            files = stitch.list_image_files(root)

            self.assertEqual([p.name for p in files], ["RGB_1.jpg", "RGB_2.jpg", "RGB_10.jpg", "RGB_11.jpeg"])

    def test_group_frames_uses_complete_groups_only(self):
        frames = [Path(f"{i}.jpg") for i in range(34)]

        groups = stitch.group_frames(frames, frames_per_panorama=16)

        self.assertEqual(len(groups), 2)
        self.assertEqual(groups[0], frames[:16])
        self.assertEqual(groups[1], frames[16:32])


if __name__ == "__main__":
    unittest.main()
