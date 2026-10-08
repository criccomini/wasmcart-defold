#!/usr/bin/env bash
# Run from any directory. Uses the vendored ZIP reader and local C/C++ compilers.
set -euo pipefail
cd "$(dirname "$0")/.."
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include"
ln -s "$PWD/engine/resource/src" "$work/include/resource"
cc="${CC:-cc}"
cxx="${CXX:-c++}"
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-unused-function -Wno-missing-field-initializers
  -fsanitize=address,undefined -ffunction-sections -fdata-sections
  -DDLIB_LOG_DOMAIN=\"TEST\" -DDM_NO_THREAD_SUPPORT
  -Iengine/dlib/src -Iengine/resource/src -Iengine/extension/src
  -Iengine/lua/src -I"$work/include")
case "$(uname -s)" in
  Darwin) link=(-Wl,-dead_strip) ;;
  *) link=(-Wl,--gc-sections) ;;
esac
"$cc" -std=c99 -O1 -g -fsanitize=address,undefined \
  -DMINIZ_NO_TIME -DZIP_ENABLE_INFLATE=1 -DZIP_ENABLE_DEFLATE=0 \
  -DZIP_HAVE_SYMLINK=0 -Iengine/dlib/src/zip \
  -c engine/dlib/src/zip/zip.c -o "$work/zip.o"
"$cxx" "${flags[@]}" "${link[@]}" \
  engine/dlib/src/test/test_zip_wasmcart.cpp \
  engine/dlib/src/dlib/zip.cpp engine/dlib/src/dlib/path.cpp \
  engine/dlib/src/dlib/uri.cpp engine/dlib/src/dlib/dstrings.cpp \
  engine/dlib/src/dlib/mutex_web.cpp "$work/zip.o" -o "$work/test_mounts"
"$work/test_mounts"
"$cxx" "${flags[@]}" -fsyntax-only engine/dlib/src/dlib/zip_wasmcart.cpp
"$cxx" "${flags[@]}" -fsyntax-only engine/liveupdate/src/liveupdate.cpp
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/tests -p test_split_liveupdate.py
