#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/test_common.sh
source "$SCRIPT_DIR/lib/test_common.sh"

usage()
{
    cat <<'EOF'
Usage: scripts/test_build.sh [--run-id ID] [--jobs N]

Builds DMX and DMX_test in separate fresh result directories.
Results are written below test-results/ unless DMX_TEST_RESULTS_ROOT is set.
EOF
}

RUN_ID="${DMX_TEST_RUN_ID:-$(dmx_default_run_id TEST_build)}"
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
dmx_require_command sha256sum
QMAKE_BIN="$(dmx_qmake_bin)"
export QMAKE_BIN

dmx_prepare_results "$RUN_ID" build
dmx_write_metadata "$RUN_ID" build
dmx_install_status_trap

build_target()
{
    local project_file="$1"
    local target="$2"
    local build_dir="$DMX_TEST_RESULT_DIR/work/$target"

    mkdir -p "$build_dir"
    cd "$build_dir"
    "$QMAKE_BIN" "$DMX_PROJECT_DIR/$project_file" -spec linux-g++
    make -j"$JOBS"
    test -x "$build_dir/$target"
    sha256sum "$build_dir/$target" >>"$DMX_TEST_RESULT_DIR/binaries.sha256"
}

dmx_run_logged build_dmx build_target DMX.pro DMX
dmx_run_logged build_dmx_test build_target DMX_test.pro DMX_test

DMX_TEST_SUITE_PASSED=1
printf '\nBUILD TEST PASS\nRun ID: %s\nResults: %s\n' "$RUN_ID" "$DMX_TEST_RESULT_DIR"
