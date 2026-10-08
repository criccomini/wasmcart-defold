#!/usr/bin/env bash
# Host-only checks. No engine wasm build or SDK downloads.
set -euo pipefail
cd "$(dirname "$0")/.."
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/tests -p test_wasmcart_font_formats.py -v
# Reuse the console development image's native GLES and software EGL.
# The caller must already have this image. It is never pulled by this script.
docker image inspect couchmix-dev >/dev/null
docker run --rm --pull=never --user "$(id -u):$(id -g)" --entrypoint sh \
  -e LIBGL_ALWAYS_SOFTWARE=1 -v "$PWD":/fork:ro -w /fork couchmix-dev -ec '
    work=$(mktemp -d)
    trap '\''rm -rf "$work"'\'' EXIT
    c++ -std=c++11 -Wall -Wextra -Werror -U__linux__ \
      -D__EMSCRIPTEN__ -DDM_PLATFORM_WASMCART -DGL_ES_VERSION_2_0 \
      engine/graphics/src/test/test_wasmcart_font_upload.cpp \
      -lEGL -lGLESv2 -o "$work/font_upload"
    "$work/font_upload"
  '
