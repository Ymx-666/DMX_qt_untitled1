#!/usr/bin/env bash

# Shared helpers for DMX test entrypoints. Callers must enable strict mode.

DMX_PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

dmx_require_command()
{
    local command_name="$1"
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "[ERROR] required command not found: $command_name" >&2
        return 2
    fi
}

dmx_qmake_bin()
{
    if [ -n "${QMAKE_BIN:-}" ]; then
        printf '%s\n' "$QMAKE_BIN"
    elif [ -x /usr/bin/qmake ]; then
        printf '%s\n' /usr/bin/qmake
    elif command -v qmake >/dev/null 2>&1; then
        command -v qmake
    else
        echo "[ERROR] qmake not found; set QMAKE_BIN or install qt5-qmake" >&2
        return 2
    fi
}

dmx_default_jobs()
{
    if [ -n "${DMX_TEST_JOBS:-}" ]; then
        printf '%s\n' "$DMX_TEST_JOBS"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '2\n'
    fi
}

dmx_default_run_id()
{
    local prefix="$1"
    printf '%s_%s\n' "$prefix" "$(date +%Y%m%d_%H%M%S)"
}

dmx_prepare_results()
{
    local run_id="$1"
    local suite="$2"
    local results_root="${DMX_TEST_RESULTS_ROOT:-$DMX_PROJECT_DIR/test-results}"

    DMX_TEST_RESULT_DIR="$results_root/$run_id/$suite"
    if [ -e "$DMX_TEST_RESULT_DIR" ]; then
        echo "[ERROR] result directory already exists: $DMX_TEST_RESULT_DIR" >&2
        return 2
    fi
    mkdir -p "$DMX_TEST_RESULT_DIR/logs"
    export DMX_TEST_RESULT_DIR
}

dmx_write_metadata()
{
    local run_id="$1"
    local suite="$2"
    {
        printf 'run_id=%s\n' "$run_id"
        printf 'suite=%s\n' "$suite"
        printf 'git_commit=%s\n' "$(git -C "$DMX_PROJECT_DIR" rev-parse HEAD)"
        printf 'git_branch=%s\n' "$(git -C "$DMX_PROJECT_DIR" branch --show-current)"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'host=%s\n' "$(hostname)"
        printf 'kernel=%s\n' "$(uname -sr)"
        printf 'python=%s\n' "$(python3 --version 2>&1 || true)"
    } >"$DMX_TEST_RESULT_DIR/metadata.txt"
}

dmx_write_status()
{
    local rc="$1"
    local state=failed
    if [ "$DMX_TEST_SUITE_PASSED" -eq 1 ] && [ "$rc" -eq 0 ]; then
        state=passed
    fi
    printf 'status=%s\nexit_code=%s\nfinished_utc=%s\n' \
        "$state" "$rc" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >"$DMX_TEST_RESULT_DIR/status.txt"
}

dmx_finish_status()
{
    local rc=$?
    dmx_write_status "$rc"
    trap - EXIT
    exit "$rc"
}

dmx_install_status_trap()
{
    DMX_TEST_SUITE_PASSED=0
    trap dmx_finish_status EXIT
}

dmx_run_logged()
{
    local step="$1"
    shift
    local log="$DMX_TEST_RESULT_DIR/logs/$step.log"
    local rc

    printf '\n[%s] START %s\n' "$(date +%H:%M:%S)" "$step"
    if "$@" > >(tee "$log") 2>&1; then
        printf '[%s] PASS  %s\n' "$(date +%H:%M:%S)" "$step"
        return 0
    else
        rc=$?
    fi

    printf '[%s] FAIL  %s (exit=%s, log=%s)\n' \
        "$(date +%H:%M:%S)" "$step" "$rc" "$log" >&2
    return "$rc"
}
