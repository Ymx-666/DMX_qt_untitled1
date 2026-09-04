#!/usr/bin/env python3
"""由DMX全景目标坐标计算理想/标定后的视线角。

本工具只处理已经完成旋转、镜像和双通道纵向配准后的全景坐标。
仅提供FOV时，输出属于模型估算值；若要称为真实绝对角，还需线阵空间轴射线参数、
畸变校正、光轴姿态及安装零位标定。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path
from typing import Any, Iterable


def relative_elevation_deg(
    v_px: float,
    height_px: int,
    fov_deg: float,
    model: str,
    principal_y_px: float,
    focal_y_px: float | None,
) -> tuple[float, float | None]:
    if height_px < 2:
        raise ValueError("图像高度必须不小于2像素")
    if not 0.0 < fov_deg < 180.0:
        raise ValueError("视场角必须在0°和180°之间")
    if model == "linear":
        angle = (0.5 - v_px / float(height_px - 1)) * fov_deg
        return angle, None
    fy = focal_y_px
    if fy is None:
        fy = height_px / (2.0 * math.tan(math.radians(fov_deg) / 2.0))
    if fy <= 0.0:
        raise ValueError("纵向像素焦距必须大于0")
    angle = math.degrees(math.atan2(principal_y_px - v_px, fy))
    return angle, fy


def circular_pixel_span(x1: float, x2: float, width: int) -> float:
    direct = abs(x2 - x1) % width
    return min(direct, width - direct)


def calculate(args: argparse.Namespace, record: dict[str, Any]) -> dict[str, Any]:
    height = args.height
    width = args.width
    cy = args.cy if args.cy is not None else (height - 1) / 2.0

    pano_y = float(record["panoY"])
    corrected_y = pano_y + args.y_offset_px
    rel, fy = relative_elevation_deg(
        corrected_y,
        height,
        args.fov_deg,
        args.model,
        cy,
        args.fy_px,
    )
    absolute = rel + args.axis_pitch_deg if args.axis_pitch_deg is not None else None

    pano_x = record.get("panoX")
    raw_azimuth = None
    if pano_x is not None:
        raw_azimuth = (float(pano_x) / float(width) * 360.0 + args.azimuth_zero_deg) % 360.0

    top = record.get("panoBoxY1")
    bottom = record.get("panoBoxY2")
    angular_height = None
    if top is not None and bottom is not None:
        top_angle, _ = relative_elevation_deg(
            float(top) + args.y_offset_px, height, args.fov_deg,
            args.model, cy, args.fy_px,
        )
        bottom_angle, _ = relative_elevation_deg(
            float(bottom) + args.y_offset_px, height, args.fov_deg,
            args.model, cy, args.fy_px,
        )
        angular_height = abs(top_angle - bottom_angle)

    left = record.get("panoBoxX1")
    right = record.get("panoBoxX2")
    pano_angular_width = None
    if left is not None and right is not None:
        pano_angular_width = circular_pixel_span(float(left), float(right), width) * 360.0 / width

    calibrated_intrinsics = args.fy_px is not None and args.cy is not None
    result = {
        "source": record.get("source", record.get("file", "")),
        "stream": record.get("stream", ""),
        "rxMs": record.get("rxMs", ""),
        "panoX": pano_x if pano_x is not None else "",
        "panoY": pano_y,
        "correctedPanoY": corrected_y,
        "rawAzimuthDeg": raw_azimuth,
        "relativeElevationDeg": rel,
        "absoluteElevationDeg": absolute,
        "verticalAngularSizeDeg": angular_height,
        "panoramaAngularWidthDeg": pano_angular_width,
        "model": args.model,
        "fovDeg": args.fov_deg,
        "principalYpx": cy,
        "focalYpx": fy,
        "axisPitchDeg": args.axis_pitch_deg,
        "calibrationLevel": (
            "intrinsics_plus_axis_pitch"
            if calibrated_intrinsics and args.axis_pitch_deg is not None
            else "intrinsics_only"
            if calibrated_intrinsics
            else "fov_only_model_estimate"
        ),
    }
    return result


def read_jsonl(path: Path) -> Iterable[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number} 不是有效JSON: {exc}") from exc
            if "panoY" not in record:
                raise ValueError(f"{path}:{line_number} 缺少panoY字段")
            yield record


def format_value(value: Any) -> Any:
    if isinstance(value, float):
        return f"{value:.8f}"
    if value is None:
        return ""
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="计算DMX目标的方位角、相对高度角和可选绝对高度角")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--pano-y", type=float, help="单个目标的全景纵坐标")
    source.add_argument("--input-jsonl", type=Path, help="包含panoY等字段的候选manifest.jsonl")
    parser.add_argument("--pano-x", type=float, help="单个目标的全景横坐标")
    parser.add_argument("--box-y1", type=float, help="目标框上边缘全景纵坐标")
    parser.add_argument("--box-y2", type=float, help="目标框下边缘全景纵坐标")
    parser.add_argument("--box-x1", type=float, help="目标框左边缘全景横坐标")
    parser.add_argument("--box-x2", type=float, help="目标框右边缘全景横坐标")
    parser.add_argument("--height", type=int, default=4096, help="有效全景高度，即旋转后的线阵有效Width，默认4096")
    parser.add_argument("--width", type=int, default=65536, help="有效全景宽度，默认65536")
    parser.add_argument("--fov-deg", type=float, default=26.88, help="旋转后纵向有效视场角，默认26.88°")
    parser.add_argument("--model", choices=("pinhole", "linear"), default="pinhole", help="角度模型，默认针孔模型")
    parser.add_argument("--cy", type=float, help="标定主点纵坐标；默认(H-1)/2")
    parser.add_argument("--fy-px", type=float, help="标定纵向像素焦距；省略时由FOV估算")
    parser.add_argument("--y-offset-px", type=float, default=0.0, help="该通道纵向配准修正量，vu=panoY+offset")
    parser.add_argument("--axis-pitch-deg", type=float, help="光轴相对水平面的俯仰角；仅适用于滚转可忽略的简化情况")
    parser.add_argument("--azimuth-zero-deg", type=float, default=0.0, help="全景零位到约定方位零位的校正量")
    parser.add_argument("--output-csv", type=Path, help="批量输出CSV；省略时写到标准输出")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.input_jsonl is not None:
        records = read_jsonl(args.input_jsonl)
    else:
        records = [{
            "panoY": args.pano_y,
            "panoX": args.pano_x,
            "panoBoxY1": args.box_y1,
            "panoBoxY2": args.box_y2,
            "panoBoxX1": args.box_x1,
            "panoBoxX2": args.box_x2,
        }]

    results = [calculate(args, record) for record in records]
    if not results:
        print("输入文件中没有记录", file=sys.stderr)
        return 2

    if args.input_jsonl is None and args.output_csv is None:
        print(json.dumps({key: format_value(value) for key, value in results[0].items()}, ensure_ascii=False, indent=2))
        return 0

    fieldnames = list(results[0].keys())
    if args.output_csv is not None:
        args.output_csv.parent.mkdir(parents=True, exist_ok=True)
        handle = args.output_csv.open("w", encoding="utf-8-sig", newline="")
        close_handle = True
    else:
        handle = sys.stdout
        close_handle = False
    try:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            writer.writerow({key: format_value(value) for key, value in result.items()})
    finally:
        if close_handle:
            handle.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
