#!/bin/bash
# Linux 构建脚本。从任何位置执行都会构建到本脚本同级的 build_linux/。

set -e
cd "$(dirname "$0")"

mkdir -p build_linux
cd build_linux

echo "[1/2] qmake ..."
QMAKE_BIN="${QMAKE_BIN:-/usr/bin/qmake}"
"$QMAKE_BIN" ../DMX.pro -spec linux-g++

echo "[2/2] make -j$(nproc) ..."
make -j$(nproc)

echo ""
echo "============================"
echo "Build OK"
echo "EXE: $(pwd)/DMX"
echo ""
echo "运行: $(pwd)/DMX"
echo "============================"
