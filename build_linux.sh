#!/bin/bash
# Linux 构建脚本。从任何位置执行都会构建到本脚本同级的 build_linux/。

set -e
cd "$(dirname "$0")"

mkdir -p build_linux
cd build_linux

echo "[1/2] qmake ..."
qmake ../untitled1.pro -spec linux-g++

echo "[2/2] make -j$(nproc) ..."
make -j$(nproc)

echo ""
echo "============================"
echo "Build OK"
echo "EXE: $(pwd)/untitled1"
echo ""
echo "运行: $(pwd)/untitled1"
echo "============================"
