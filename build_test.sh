#!/bin/bash
# Build the isolated replay executable from the same sources as the live DMX app.

set -e
cd "$(dirname "$0")"

mkdir -p build_linux_test
cd build_linux_test

QMAKE_BIN="${QMAKE_BIN:-/usr/bin/qmake}"
"$QMAKE_BIN" ../DMX_test.pro -spec linux-g++
make -j"$(nproc)"

echo "Build OK: $(pwd)/DMX_test"
