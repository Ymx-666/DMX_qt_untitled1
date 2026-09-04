#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

latexmk -xelatex -interaction=nonstopmode -file-line-error \
  -output-directory=build 'DMX软硬件框架图.tex'

pdftoppm -f 1 -l 1 -singlefile -png -r 240 \
  'build/DMX软硬件框架图.pdf' 'build/DMX软硬件框架图'

convert 'build/DMX软硬件框架图.png' -trim +repage \
  -bordercolor white -border 48x48 +repage 'build/DMX软硬件框架图-裁剪版.png'
