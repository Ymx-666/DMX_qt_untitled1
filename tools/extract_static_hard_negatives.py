#!/usr/bin/env python3
"""Extract reviewed static-clutter detections as YOLO hard negatives."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
from pathlib import Path
from typing import Any

import cv2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stream", default="RGB")
    parser.add_argument("--class-name", default="drone")
    parser.add_argument("--center-x", type=int, required=True)
    parser.add_argument("--center-y", type=int, required=True)
    parser.add_argument("--tolerance-x", type=int, default=12)
    parser.add_argument("--tolerance-y", type=int, default=8)
    parser.add_argument("--max-samples", type=int, default=32)
    return parser.parse_args()


def load_matches(args: argparse.Namespace) -> list[dict[str, Any]]:
    matches: dict[str, dict[str, Any]] = {}
    with args.manifest.open("r", encoding="utf-8") as handle:
        for line in handle:
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            path = Path(str(record.get("path", "")))
            if not path.is_file():
                continue
            if str(record.get("stream", "")).upper() != args.stream.upper():
                continue
            if str(record.get("className", "")).lower() != args.class_name.lower():
                continue
            if abs(int(record.get("panoX", -1000000)) - args.center_x) > args.tolerance_x:
                continue
            if abs(int(record.get("panoY", -1000000)) - args.center_y) > args.tolerance_y:
                continue
            matches[str(path)] = record
    return sorted(
        matches.values(),
        key=lambda item: (int(item.get("fileIdx", 0)), str(item.get("path", ""))),
    )


def select_evenly(records: list[dict[str, Any]], limit: int) -> list[dict[str, Any]]:
    if limit <= 0 or len(records) <= limit:
        return records
    indexes: list[int] = []
    for position in range(limit):
        index = round(position * (len(records) - 1) / max(1, limit - 1))
        if not indexes or index != indexes[-1]:
            indexes.append(index)
    return [records[index] for index in indexes]


def main() -> int:
    args = parse_args()
    records = load_matches(args)
    if not records:
        raise SystemExit("no matching hard-negative records")
    selected = select_evenly(records, max(1, args.max_samples))

    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f"output directory is not empty: {args.output}")
    images_dir = args.output / "images"
    labels_dir = args.output / "labels"
    images_dir.mkdir(parents=True, exist_ok=True)
    labels_dir.mkdir(parents=True, exist_ok=True)

    manifest_fields = [
        "output_image",
        "output_label",
        "source_path",
        "source",
        "stream",
        "className",
        "fileIdx",
        "panoX",
        "panoY",
        "angle",
        "yoloRawScore",
        "frameBoxX1",
        "frameBoxY1",
        "frameBoxX2",
        "frameBoxY2",
        "roiBoxX1",
        "roiBoxY1",
        "roiBoxX2",
        "roiBoxY2",
    ]
    written_rows: list[dict[str, Any]] = []
    review_tiles = []
    for index, record in enumerate(selected, 1):
        source_path = Path(str(record["path"]))
        image = cv2.imread(str(source_path), cv2.IMREAD_COLOR)
        if image is None or image.shape[:2] != (512, 512):
            continue
        output_name = f"static_pole_negative_{index:03d}_{source_path.name}"
        output_image = images_dir / output_name
        output_label = labels_dir / f"{Path(output_name).stem}.txt"
        shutil.copy2(source_path, output_image)
        output_label.write_text("", encoding="ascii")

        row = {field: record.get(field, "") for field in manifest_fields}
        row["output_image"] = str(output_image)
        row["output_label"] = str(output_label)
        row["source_path"] = str(source_path)
        written_rows.append(row)

        tile = cv2.resize(image, (192, 192), interpolation=cv2.INTER_AREA)
        caption = f"{record.get('fileIdx', '')} conf={float(record.get('yoloRawScore', 0.0)):.3f}"
        cv2.rectangle(tile, (0, 164), (191, 191), (0, 0, 0), thickness=-1)
        cv2.putText(tile, caption, (5, 183), cv2.FONT_HERSHEY_SIMPLEX, 0.42, (255, 255, 255), 1)
        review_tiles.append(tile)

    if not written_rows:
        raise SystemExit("matching records exist, but no valid 512x512 images were written")

    with (args.output / "manifest.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=manifest_fields)
        writer.writeheader()
        writer.writerows(written_rows)

    columns = min(6, len(review_tiles))
    rows = (len(review_tiles) + columns - 1) // columns
    sheet = cv2.vconcat([
        cv2.hconcat(
            review_tiles[row * columns : (row + 1) * columns]
            + [review_tiles[0] * 0] * (columns - len(review_tiles[row * columns : (row + 1) * columns]))
        )
        for row in range(rows)
    ])
    cv2.imwrite(str(args.output / "review_contact_sheet.jpg"), sheet, [cv2.IMWRITE_JPEG_QUALITY, 92])

    summary = {
        "sourceManifest": str(args.manifest),
        "matchedUniqueImages": len(records),
        "selectedImages": len(written_rows),
        "selection": {
            "stream": args.stream.upper(),
            "className": args.class_name.lower(),
            "centerX": args.center_x,
            "centerY": args.center_y,
            "toleranceX": args.tolerance_x,
            "toleranceY": args.tolerance_y,
        },
        "trainingUsage": "Negative images have matching empty YOLO label files.",
    }
    (args.output / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    (args.output / "README.md").write_text(
        "# Static pole hard negatives\n\n"
        "The files under `images/` are clean 512x512 detector crops. Matching files "
        "under `labels/` are intentionally empty YOLO labels. This set contains one "
        "reviewed fixed utility-pole false-positive cluster and must be merged with "
        "the normal positive/negative training split; it is not a standalone dataset.\n\n"
        "`manifest.csv` preserves the original candidate path, frame index, panorama "
        "position, model confidence, and boxes. `review_contact_sheet.jpg` is for "
        "visual review only and must not be used for training.\n",
        encoding="ascii",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
