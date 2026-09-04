#!/bin/bash

set -e

PROJECT_DIR="/home/sht/work/DMX_qt"
DMX_BINARY="$PROJECT_DIR/build_linux/DMX"
RUNTIME_DIR="/mnt/dmx4t/data/runtime"
YOLO_MODEL="/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights5/weights/best.onnx"
YOLO_PYTHON="$PROJECT_DIR/.venv/bin/python"

notify_error()
{
    local message="$1"
    if command -v notify-send >/dev/null 2>&1; then
        notify-send -u critical "DMX" "$message"
    fi
    printf 'DMX: %s\n' "$message" >&2
}

if ! mountpoint -q /mnt/dmx_share; then
    notify_error "相机共享盘尚未挂载到 /mnt/dmx_share。"
    exit 1
fi
if ! mountpoint -q /mnt/dmx4t; then
    notify_error "数据盘尚未挂载到 /mnt/dmx4t。"
    exit 1
fi
if [ ! -r "$YOLO_MODEL" ]; then
    notify_error "weights5 模型不存在或不可读：$YOLO_MODEL"
    exit 1
fi
if [ ! -x "$YOLO_PYTHON" ]; then
    notify_error "YOLO Python 环境不可用：$YOLO_PYTHON"
    exit 1
fi

mkdir -p "$RUNTIME_DIR"
exec 9>"$RUNTIME_DIR/dmx.lock"
if ! flock -n 9; then
    notify_error "DMX 已经在运行。"
    exit 1
fi

if [ ! -x "$DMX_BINARY" ]; then
    if ! "$PROJECT_DIR/build_linux.sh" >"$RUNTIME_DIR/build_linux.log" 2>&1; then
        notify_error "DMX 编译失败，日志：$RUNTIME_DIR/build_linux.log"
        exit 1
    fi
fi

export DMX_REPLAY_MODE=0
export DMX_PROJECT_DIR="$PROJECT_DIR"
export DMX_SHARE_MOUNT=/mnt/dmx_share
export DMX_DATA_ROOT=/mnt/dmx4t/data
export DMX_DETECT_SAVE_ROOT=/mnt/dmx4t/data/candidates
export DMX_SKY_MASK_SAVE_ROOT=/mnt/dmx4t/data/sky_masks
export DMX_MANUAL_NEGATIVE_ROOT=/mnt/dmx4t/data/manual_negative_samples
export DMX_MANUAL_NEGATIVE_FEEDBACK=1
export DMX_DETECT_STREAM=BOTH
export DMX_DETECT_SKY_SHRINK_PIXELS=64
export DMX_DETECT_SKY_GEOMETRY_CLEANUP=1
export DMX_TRADITIONAL_DIAGNOSTIC_ONLY=1
export DMX_TRADITIONAL_DOWNSCALE=2
export DMX_DIRECT_YOLO=1
export DMX_DIRECT_YOLO_PYTHON="$YOLO_PYTHON"
export DMX_YOLO_MODEL="$YOLO_MODEL"
export DMX_YOLO_INPUT_SIZE=512
export DMX_DIRECT_YOLO_CLASS_NAMES="drone,bird,civilian_airliners"
export DMX_DIRECT_YOLO_HIGH_THRESHOLD=0.12
export DMX_DIRECT_YOLO_LOW_THRESHOLD=0.05
export DMX_DIRECT_YOLO_BIRD_RATIO=1.10
export DMX_DIRECT_YOLO_SKY_COVERAGE=0.15
export DMX_DIRECT_YOLO_FRAME_BUDGET_MS=240
export DMX_DIRECT_YOLO_CATCHUP_BUDGET_MS=120
export DMX_DIRECT_YOLO_MAX_QUEUE_DELAY_MS=450
export DMX_DIRECT_YOLO_MAX_OUTPUT=3
export DMX_STATIC_CLUTTER=0

cd "$PROJECT_DIR"
exec "$DMX_BINARY"
