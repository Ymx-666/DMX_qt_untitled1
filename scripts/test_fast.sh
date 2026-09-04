#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/test_common.sh
source "$SCRIPT_DIR/lib/test_common.sh"

usage()
{
    cat <<'EOF'
Usage: scripts/test_fast.sh [--run-id ID] [--jobs N]

Runs JSON and shell validation, Python tests, and the three Qt unit suites.
Results are written below test-results/ unless DMX_TEST_RESULTS_ROOT is set.
EOF
}

RUN_ID="${DMX_TEST_RUN_ID:-$(dmx_default_run_id TEST_fast)}"
JOBS="$(dmx_default_jobs)"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --run-id)
            RUN_ID="${2:?missing value for --run-id}"
            shift 2
            ;;
        --jobs)
            JOBS="${2:?missing value for --jobs}"
            shift 2
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

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || {
    echo "[ERROR] --jobs must be a positive integer" >&2
    exit 2
}

dmx_require_command git
dmx_require_command make
dmx_require_command python3
QMAKE_BIN="$(dmx_qmake_bin)"
export QMAKE_BIN

dmx_prepare_results "$RUN_ID" fast
dmx_write_metadata "$RUN_ID" fast
dmx_install_status_trap
export PYTHONPYCACHEPREFIX="$DMX_TEST_RESULT_DIR/python-cache"

validate_json()
{
    local path
    while IFS= read -r -d '' path; do
        python3 -m json.tool "$DMX_PROJECT_DIR/$path" >/dev/null
        printf 'valid JSON: %s\n' "$path"
    done < <(git -C "$DMX_PROJECT_DIR" ls-files -co --exclude-standard -z '*.json')
}

validate_shell()
{
    local path
    while IFS= read -r -d '' path; do
        bash -n "$DMX_PROJECT_DIR/$path"
        printf 'valid shell: %s\n' "$path"
    done < <(git -C "$DMX_PROJECT_DIR" ls-files -co --exclude-standard -z '*.sh')
}

validate_python_dependencies()
{
    python3 - <<'PY'
import cv2
import numpy
import pytest

print(f"opencv={cv2.__version__}")
print(f"numpy={numpy.__version__}")
print(f"pytest={pytest.__version__}")
PY
}

run_python_tests()
{
    cd "$DMX_PROJECT_DIR"
    python3 -m compileall -q tools tests
    python3 -m pytest -q tests \
        --junitxml="$DMX_TEST_RESULT_DIR/python.junit.xml"
}

run_qt_test()
{
    local project_file="$1"
    local target="$2"
    local build_dir="$DMX_TEST_RESULT_DIR/build/$target"

    mkdir -p "$build_dir"
    cd "$build_dir"
    "$QMAKE_BIN" "$DMX_PROJECT_DIR/$project_file" -spec linux-g++
    make -j"$JOBS"
    QT_QPA_PLATFORM=offscreen "./$target" \
        -o -,txt \
        -o "$DMX_TEST_RESULT_DIR/$target.junit.xml,junitxml"
}

run_git_checks()
{
    git -C "$DMX_PROJECT_DIR" diff --check
    git -C "$DMX_PROJECT_DIR" diff --cached --check
}

dmx_run_logged git_diff_check run_git_checks
dmx_run_logged json_validation validate_json
dmx_run_logged shell_syntax validate_shell
dmx_run_logged python_dependencies validate_python_dependencies
dmx_run_logged python_tests run_python_tests
dmx_run_logged appconfig_qttest run_qt_test appconfig_tests.pro appconfig_tests
dmx_run_logged panoramacache_qttest run_qt_test panoramacache_tests.pro panoramacache_tests
dmx_run_logged manualnegative_qttest run_qt_test manualnegativestore_tests.pro manualnegativestore_tests

DMX_TEST_SUITE_PASSED=1
printf '\nFAST TEST PASS\nRun ID: %s\nResults: %s\n' "$RUN_ID" "$DMX_TEST_RESULT_DIR"
