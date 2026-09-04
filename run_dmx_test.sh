#!/bin/bash

set -e

PROJECT_DIR="/home/sht/work/DMX_qt"
TEST_BINARY="$PROJECT_DIR/build_linux_test/DMX_test"
REPLAY_TOOL="$PROJECT_DIR/tools/replay_raw_udp_20260723.py"
SOURCE_LOG_ROOT="/mnt/dmx4t/data/raw_log/20260723"
REMOTE_IMAGE_MOUNT="/mnt/dmx_share"
LOCAL_IMAGE_MOUNT="/mnt/dmx4t/data/replay_sources/baseline_20260723"
LOCAL_IMAGE_MANIFEST="$LOCAL_IMAGE_MOUNT/local_replay_manifest.json"
TEST_DATA_ROOT="/mnt/dmx4t/data/dmx_test"
YOLO_MODEL="/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights5/weights/best.onnx"
PATH_PORT=18001
CONTROL_PORT=18501
REPLY_PORT=15002

notify_error()
{
    local message="$1"
    echo "dmx_test: $message" >&2
    if command -v zenity >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
        zenity --error --title="dmx_test 无法启动" --width=520 --text="$message" >/dev/null 2>&1 &
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send -u critical "dmx_test" "$message"
    fi
}

mkdir -p "$TEST_DATA_ROOT/runtime"
exec 9>"$TEST_DATA_ROOT/runtime/dmx_test.lock"
if ! flock -n 9; then
    notify_error "dmx_test is already running."
    exit 1
fi

if [ ! -d "$SOURCE_LOG_ROOT" ]; then
    notify_error "Source log directory is missing: $SOURCE_LOG_ROOT"
    exit 1
fi

SOURCE_IMAGE_MOUNT=""
REPLAY_IMAGE_ARGS=()
if [ -f "$LOCAL_IMAGE_MANIFEST" ] \
    && [ -d "$LOCAL_IMAGE_MOUNT/raw/20260723/BW" ] \
    && [ -d "$LOCAL_IMAGE_MOUNT/raw/20260723/RGB" ]; then
    SOURCE_IMAGE_MOUNT="$LOCAL_IMAGE_MOUNT"
    REPLAY_IMAGE_ARGS+=(--available-only)
elif mountpoint -q "$REMOTE_IMAGE_MOUNT" \
    && [ -d "$REMOTE_IMAGE_MOUNT/raw/20260723/BW" ] \
    && [ -d "$REMOTE_IMAGE_MOUNT/raw/20260723/RGB" ]; then
    SOURCE_IMAGE_MOUNT="$REMOTE_IMAGE_MOUNT"
else
    if [ -r /sys/class/net/eno2/carrier ] && [ "$(cat /sys/class/net/eno2/carrier)" != "1" ]; then
        notify_error "本地回放基准尚未生成，设备共享盘也不可用；有线网卡 eno2 未检测到物理连接。请先恢复网线并挂载共享盘，或运行本地 AB 数据恢复工具。"
    else
        notify_error "本地回放基准尚未生成，设备共享盘也未挂载。请先运行 mount_share.sh，或运行本地 AB 数据恢复工具。"
    fi
    exit 1
fi
if [ ! -r "$YOLO_MODEL" ]; then
    notify_error "weights5 model is missing or unreadable: $YOLO_MODEL"
    exit 1
fi

if [ ! -x "$TEST_BINARY" ]; then
    if ! "$PROJECT_DIR/build_test.sh" >"$TEST_DATA_ROOT/runtime/build_test.log" 2>&1; then
        notify_error "DMX_test build failed. See $TEST_DATA_ROOT/runtime/build_test.log"
        exit 1
    fi
fi

RUN_ID="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$TEST_DATA_ROOT/runs/$RUN_ID"
mkdir -p "$RUN_DIR"

python3 "$REPLAY_TOOL" \
    --log-root "$SOURCE_LOG_ROOT" \
    --share-mount "$SOURCE_IMAGE_MOUNT" \
    "${REPLAY_IMAGE_ARGS[@]}" \
    --dst-ip 127.0.0.1 \
    --dst-port "$PATH_PORT" \
    --control-ip 127.0.0.1 \
    --control-port "$CONTROL_PORT" \
    --reply-ip 127.0.0.1 \
    --reply-port "$REPLY_PORT" \
    --revolution-seconds 8 \
    --segments 16 \
    --pair-offset-ms 102 \
    >"$RUN_DIR/replay_sender.log" 2>&1 &
REPLAY_PID=$!

cleanup()
{
    if kill -0 "$REPLAY_PID" 2>/dev/null; then
        kill "$REPLAY_PID" 2>/dev/null || true
        wait "$REPLAY_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

READY=0
for _ in $(seq 1 100); do
    if ! kill -0 "$REPLAY_PID" 2>/dev/null; then
        notify_error "Replay sender exited during startup. See $RUN_DIR/replay_sender.log"
        exit 1
    fi
    if ss -lun "sport = :$CONTROL_PORT" | tail -n +2 | grep -q .; then
        READY=1
        break
    fi
    sleep 0.05
done
if [ "$READY" -ne 1 ]; then
    notify_error "Replay sender did not open control port $CONTROL_PORT."
    exit 1
fi

export DMX_REPLAY_MODE=1
export DMX_PROJECT_DIR="$PROJECT_DIR"
export DMX_PATH_PORT="$PATH_PORT"
export DMX_DEVICE_IP=127.0.0.1
export DMX_CMD_PORT_SEND="$CONTROL_PORT"
export DMX_CMD_PORT_REPLY="$REPLY_PORT"
export DMX_SHARE_MOUNT="$SOURCE_IMAGE_MOUNT"
export DMX_DATA_ROOT="$TEST_DATA_ROOT"
export DMX_SAVE_ROOT="$TEST_DATA_ROOT/saves"
export DMX_REC_ROOT="$TEST_DATA_ROOT/recordings"
export DMX_LOG_ROOT="$TEST_DATA_ROOT/logs"
export DMX_RAW_LOG_ROOT="$TEST_DATA_ROOT/raw_log"
export DMX_DETECT_SAVE_ROOT="$TEST_DATA_ROOT/candidates"
export DMX_MANUAL_NEGATIVE_ROOT=/mnt/dmx4t/data/manual_negative_samples
export DMX_MANUAL_NEGATIVE_FEEDBACK=1
export DMX_DETECT_STREAM=BW
export DMX_TRADITIONAL_DIAGNOSTIC_ONLY=1
export DMX_TRADITIONAL_DOWNSCALE=2
export DMX_CAMERA_VERTICAL_FOV_DEG=26.88
export DMX_ANGLE_LOOKUP=0
export DMX_DETECT_SKY_SHRINK_PIXELS=64
export DMX_DETECT_SKY_GEOMETRY_CLEANUP=1
export DMX_TEST_SKY_MASK_BW="$TEST_DATA_ROOT/analysis/sky_mask_runtime_baseline_20260723/bw_mask_geometry.png"
export DMX_TEST_SKY_MASK_RGB="$TEST_DATA_ROOT/analysis/sky_mask_runtime_baseline_20260723/rgb_mask_geometry.png"
export DMX_DIRECT_YOLO=1
export DMX_DIRECT_YOLO_PYTHON="$PROJECT_DIR/.venv/bin/python"
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
"$TEST_BINARY"
