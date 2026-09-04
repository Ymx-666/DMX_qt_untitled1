#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/test_common.sh
source "$SCRIPT_DIR/lib/test_common.sh"

usage()
{
    cat <<'EOF'
Usage: scripts/test_replay_smoke.sh [options]

Options:
  --run-id ID       Override the generated Run ID.
  --duration SEC    Application runtime before controlled termination (default: 45).
  --build           Build build_linux_test/DMX_test first.
  -h, --help        Show this help.

Environment overrides:
  DMX_REPLAY_LOG_ROOT       Raw 20260723 JSONL root.
  DMX_REPLAY_IMAGE_ROOT     Local replay image mount.
  DMX_YOLO_MODEL            ONNX model path.
  DMX_TEST_PYTHON           Python with ONNX runtime dependencies.
  DMX_TEST_MASK_BW/RGB      Prebuilt panorama sky masks.
  DMX_TEST_RESULTS_ROOT     Result root (default: project/test-results).
EOF
}

RUN_ID="${DMX_TEST_RUN_ID:-$(dmx_default_run_id TEST_replay_smoke)}"
DURATION=45
BUILD_FIRST=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --run-id)
            RUN_ID="${2:?missing value for --run-id}"
            shift 2
            ;;
        --duration)
            DURATION="${2:?missing value for --duration}"
            shift 2
            ;;
        --build)
            BUILD_FIRST=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ "$DURATION" =~ ^[1-9][0-9]*$ ]] || {
    echo "[ERROR] --duration must be a positive integer" >&2
    exit 2
}

dmx_require_command grep
dmx_require_command python3
dmx_require_command ss
dmx_require_command timeout

LOG_ROOT="${DMX_REPLAY_LOG_ROOT:-/mnt/dmx4t/data/raw_log/20260723}"
IMAGE_ROOT="${DMX_REPLAY_IMAGE_ROOT:-/mnt/dmx4t/data/replay_sources/baseline_20260723}"
MODEL="${DMX_YOLO_MODEL:-/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights5/weights/best.onnx}"
YOLO_PYTHON="${DMX_TEST_PYTHON:-$DMX_PROJECT_DIR/.venv/bin/python}"
MASK_BW="${DMX_TEST_MASK_BW:-/mnt/dmx4t/data/dmx_test/analysis/sky_mask_runtime_baseline_20260723/bw_mask_geometry.png}"
MASK_RGB="${DMX_TEST_MASK_RGB:-/mnt/dmx4t/data/dmx_test/analysis/sky_mask_runtime_baseline_20260723/rgb_mask_geometry.png}"
TEST_BINARY="$DMX_PROJECT_DIR/build_linux_test/DMX_test"
REPLAY_TOOL="$DMX_PROJECT_DIR/tools/replay_raw_udp_20260723.py"
PATH_PORT="${DMX_TEST_PATH_PORT:-28001}"
CONTROL_PORT="${DMX_TEST_CONTROL_PORT:-28501}"
REPLY_PORT="${DMX_TEST_REPLY_PORT:-25002}"

require_file()
{
    local path="$1"
    local description="$2"
    if [ ! -r "$path" ]; then
        echo "[ERROR] $description is missing or unreadable: $path" >&2
        return 2
    fi
}

require_executable()
{
    local path="$1"
    local description="$2"
    if [ ! -x "$path" ]; then
        echo "[ERROR] $description is missing or not executable: $path" >&2
        return 2
    fi
}

require_directory()
{
    local path="$1"
    local description="$2"
    if [ ! -d "$path" ]; then
        echo "[ERROR] $description is missing: $path" >&2
        return 2
    fi
}

require_free_udp_port()
{
    local port="$1"
    if ss -H -lun "sport = :$port" | grep -q .; then
        echo "[ERROR] UDP port is already in use: $port" >&2
        return 2
    fi
}

if [ "$BUILD_FIRST" -eq 1 ]; then
    "$DMX_PROJECT_DIR/build_test.sh"
fi

require_directory "$LOG_ROOT" "raw replay log root"
require_directory "$IMAGE_ROOT/raw/20260723/BW" "BW replay image root"
require_directory "$IMAGE_ROOT/raw/20260723/RGB" "RGB replay image root"
require_file "$MODEL" "YOLO model"
require_executable "$YOLO_PYTHON" "YOLO Python interpreter"
require_file "$MASK_BW" "BW sky mask"
require_file "$MASK_RGB" "RGB sky mask"
require_executable "$TEST_BINARY" "DMX_test executable; run build_test.sh or pass --build"
require_file "$REPLAY_TOOL" "replay sender"
require_free_udp_port "$PATH_PORT"
require_free_udp_port "$CONTROL_PORT"
require_free_udp_port "$REPLY_PORT"

dmx_prepare_results "$RUN_ID" replay-smoke
dmx_write_metadata "$RUN_ID" replay-smoke
dmx_install_status_trap

SENDER_LOG="$DMX_TEST_RESULT_DIR/logs/replay_sender.log"
CONSOLE_LOG="$DMX_TEST_RESULT_DIR/logs/dmx_test_console.log"
REPLAY_PID=""

cleanup_replay()
{
    if [ -n "$REPLAY_PID" ] && kill -0 "$REPLAY_PID" 2>/dev/null; then
        kill "$REPLAY_PID" 2>/dev/null || true
        wait "$REPLAY_PID" 2>/dev/null || true
    fi
}

finish_replay_smoke()
{
    local rc=$?
    cleanup_replay
    dmx_write_status "$rc"
    trap - EXIT
    exit "$rc"
}
trap finish_replay_smoke EXIT

python3 "$REPLAY_TOOL" \
    --log-root "$LOG_ROOT" \
    --share-mount "$IMAGE_ROOT" \
    --available-only \
    --max-pairs 256 \
    --loop \
    --dst-ip 127.0.0.1 \
    --dst-port "$PATH_PORT" \
    --control-ip 127.0.0.1 \
    --control-port "$CONTROL_PORT" \
    --reply-ip 127.0.0.1 \
    --reply-port "$REPLY_PORT" \
    --revolution-seconds 8 \
    --segments 16 \
    --pair-offset-ms 102 \
    --progress-seconds 5 \
    >"$SENDER_LOG" 2>&1 &
REPLAY_PID=$!

READY=0
for _ in $(seq 1 100); do
    if ! kill -0 "$REPLAY_PID" 2>/dev/null; then
        echo "[ERROR] replay sender exited during startup: $SENDER_LOG" >&2
        exit 1
    fi
    if ss -H -lun "sport = :$CONTROL_PORT" | grep -q .; then
        READY=1
        break
    fi
    sleep 0.05
done
if [ "$READY" -ne 1 ]; then
    echo "[ERROR] replay sender did not open UDP control port $CONTROL_PORT" >&2
    exit 1
fi

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export DMX_REPLAY_MODE=1
export DMX_REPLAY_AUTOSTART=1
export DMX_PROJECT_DIR
export DMX_PATH_PORT="$PATH_PORT"
export DMX_DEVICE_IP=127.0.0.1
export DMX_CMD_PORT_SEND="$CONTROL_PORT"
export DMX_CMD_PORT_REPLY="$REPLY_PORT"
export DMX_SHARE_MOUNT="$IMAGE_ROOT"
export DMX_DATA_ROOT="$DMX_TEST_RESULT_DIR/runtime"
export DMX_SAVE_ROOT="$DMX_TEST_RESULT_DIR/runtime/saves"
export DMX_REC_ROOT="$DMX_TEST_RESULT_DIR/runtime/recordings"
export DMX_LOG_ROOT="$DMX_TEST_RESULT_DIR/app-logs"
export DMX_RAW_LOG_ROOT="$DMX_TEST_RESULT_DIR/runtime/raw-log"
export DMX_DETECT_SAVE_ROOT="$DMX_TEST_RESULT_DIR/runtime/candidates"
export DMX_MANUAL_NEGATIVE_ROOT="$DMX_TEST_RESULT_DIR/runtime/manual-negatives"
export DMX_MANUAL_NEGATIVE_FEEDBACK=0
export DMX_DETECT_STREAM=BW
export DMX_TRADITIONAL_DIAGNOSTIC_ONLY=1
export DMX_TRADITIONAL_DOWNSCALE=2
export DMX_CAMERA_VERTICAL_FOV_DEG=26.88
export DMX_ANGLE_LOOKUP=0
export DMX_DETECT_SKY_SHRINK_PIXELS=64
export DMX_DETECT_SKY_GEOMETRY_CLEANUP=1
export DMX_TEST_SKY_MASK_BW="$MASK_BW"
export DMX_TEST_SKY_MASK_RGB="$MASK_RGB"
export DMX_DIRECT_YOLO=1
export DMX_DIRECT_YOLO_PYTHON="$YOLO_PYTHON"
export DMX_YOLO_MODEL="$MODEL"
export DMX_YOLO_INPUT_SIZE=512
export DMX_DIRECT_YOLO_CLASS_NAMES=drone,bird,civilian_airliners
export DMX_DIRECT_YOLO_HIGH_THRESHOLD=0.12
export DMX_DIRECT_YOLO_LOW_THRESHOLD=0.05
export DMX_DIRECT_YOLO_BIRD_RATIO=1.10
export DMX_DIRECT_YOLO_SKY_COVERAGE=0.15
export DMX_DIRECT_YOLO_FRAME_BUDGET_MS=240
export DMX_DIRECT_YOLO_CATCHUP_BUDGET_MS=120
export DMX_DIRECT_YOLO_MAX_QUEUE_DELAY_MS=450
export DMX_DIRECT_YOLO_MAX_OUTPUT=3
export DMX_STATIC_CLUTTER=0

set +e
timeout --signal=TERM --kill-after=10 "$DURATION" \
    "$TEST_BINARY" >"$CONSOLE_LOG" 2>&1
APP_RC=$?
set -e
if [ "$APP_RC" -ne 124 ]; then
    echo "[ERROR] DMX_test exited before the ${DURATION}s smoke window (exit=$APP_RC)" >&2
    exit 1
fi

cleanup_replay
REPLAY_PID=""

APP_LOG="$(find "$DMX_TEST_RESULT_DIR/app-logs" -type f -name '*.txt' -print -quit 2>/dev/null || true)"
require_file "$APP_LOG" "DMX application log"
grep -Fq 'DMX_REPLAY_STARTED' "$CONSOLE_LOG" || {
    echo "[ERROR] application did not receive replay start acknowledgement" >&2
    exit 1
}
grep -Fq '[replay] started' "$SENDER_LOG" || {
    echo "[ERROR] replay sender never entered the started state" >&2
    exit 1
}
grep -Fq '[RXTYPE] RGB=' "$APP_LOG" || {
    echo "[ERROR] application log has no RGB/BW receive summary" >&2
    exit 1
}
grep -Fq '[YOLO_DIRECT] ready' "$APP_LOG" || {
    echo "[ERROR] direct YOLO worker did not become ready" >&2
    exit 1
}
if grep -Eq '\[RX\([^]]+\)\].*fail=[1-9][0-9]*' "$APP_LOG"; then
    echo "[ERROR] replay produced one or more image read failures" >&2
    exit 1
fi

RX_SUMMARIES="$(grep -Fc '[RXTYPE] RGB=' "$APP_LOG")"
YOLO_RESULTS="$(grep -Fc '[YOLO_DIRECT] RGB frame=' "$APP_LOG" || true)"
YOLO_RESULTS=$((YOLO_RESULTS + $(grep -Fc '[YOLO_DIRECT] BW frame=' "$APP_LOG" || true)))
{
    printf 'duration_seconds=%s\n' "$DURATION"
    printf 'rx_summaries=%s\n' "$RX_SUMMARIES"
    printf 'yolo_results=%s\n' "$YOLO_RESULTS"
    printf 'app_log=%s\n' "$APP_LOG"
    printf 'sender_log=%s\n' "$SENDER_LOG"
} >"$DMX_TEST_RESULT_DIR/replay-summary.txt"

DMX_TEST_SUITE_PASSED=1
printf '\nREPLAY SMOKE PASS\nRun ID: %s\nResults: %s\n' "$RUN_ID" "$DMX_TEST_RESULT_DIR"
