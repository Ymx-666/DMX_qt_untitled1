#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

latexmk -xelatex -interaction=nonstopmode -file-line-error \
  -output-directory=build 'DMX说明文档.tex'
