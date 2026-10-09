#!/usr/bin/env python3
"""Run the index helpers with real dmBuffer storage and ASan/UBSan.

Requires a local C++ compiler, protoc, and Python protobuf. Uses only source
and vectormath headers vendored in this repository. It does not build an engine.
"""
import io
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    subprocess.run([str(x) for x in command], cwd=ROOT, check=True)


def main():
    protoc = os.environ.get('PROTOC') or shutil.which('protoc')
    if not protoc:
        raise SystemExit('A local protoc is required.')
    import google.protobuf  # Use an existing Python environment with protobuf.
    with tempfile.TemporaryDirectory(prefix='defold-mesh-indices-') as temp:
        work = Path(temp).resolve()
        (work / 'plugin_pb2.py').write_text('from google.protobuf.compiler.plugin_pb2 import *\n')
        (work / 'dlib.py').write_text('raise ImportError("use the DDF generator hash fallback")\n')
        plugin = work / 'ddfc'
        plugin.write_text('#!/bin/sh\nexport PYTHONDONTWRITEBYTECODE=1\nexport PYTHONPATH=' + shlex.quote(str(work)) + '\nexec ' + shlex.quote(sys.executable) + ' ' + shlex.quote(str(ROOT / 'engine/ddf/src/ddfc.py')) + ' --cxx "$@"\n')
        plugin.chmod(0o755)
        includes = ['engine/ddf/src', 'engine/gamesys/proto', 'engine/gameobject/proto',
                    'engine/render/proto', 'engine/graphics/proto', 'engine/script/src',
                    'engine/resource/proto']
        base = [protoc] + ['-I' + str(ROOT / p) for p in includes]
        run(base + ['--python_out=' + str(work), 'ddf/ddf_extensions.proto'])
        protos = ['ddf/ddf_extensions.proto', 'ddf/ddf_math.proto', 'ddf/ddf_struct.proto',
                  'gamesys/gamesys_ddf.proto', 'gamesys/mesh_ddf.proto', 'gamesys/buffer_ddf.proto',
                  'gameobject/gameobject_ddf.proto', 'gameobject/properties_ddf.proto', 'gameobject/lua_ddf.proto',
                  'render/material_ddf.proto', 'render/render_ddf.proto', 'render/font_ddf.proto',
                  'graphics/graphics_ddf.proto', 'script/lua_source_ddf.proto', 'resource/resource_ddf.proto']
        for proto in protos:
            run(base + ['--plugin=protoc-gen-ddf=' + str(plugin), '--ddf_out=' + str(work), proto])
            for suffix in ('.h', '.cpp'):
                source = work / (Path(proto).stem + suffix)
                target = work / Path(proto).with_suffix(suffix)
                target.parent.mkdir(parents=True, exist_ok=True)
                if source != target:
                    source.replace(target)
        package = ROOT / 'packages/vectormathlibrary-r1649-common.tar.gz'
        raw = package.read_bytes() if package.exists() else subprocess.check_output(
            ['git', 'show', 'HEAD:packages/vectormathlibrary-r1649-common.tar.gz'], cwd=ROOT)
        with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as archive:
            # The vendored package contains headers. Reject path escapes.
            for member in archive.getmembers():
                if not (work / member.name).resolve().is_relative_to(work):
                    raise ValueError('unsafe package path')
            archive.extractall(work, filter='data')
        libraries = [p for p in (ROOT / 'engine').iterdir() if (p / 'src').is_dir()]
        for library in libraries:
            namespace = work / library.name
            namespace.mkdir(exist_ok=True)
            for pattern in ('*.h', '*.hpp'):
                for header in (library / 'src').glob(pattern):
                    target = namespace / header.name
                    if not target.exists():
                        target.symlink_to(header)
        (work / 'dlib/profile.h').symlink_to(ROOT / 'engine/dlib/src/dlib/profile/profile.h')
        flags = ['-std=c++11', '-O1', '-g', '-fsanitize=address,undefined',
                 '-ffunction-sections', '-fdata-sections', '-DDM_PROFILE_NULL',
                 '-DDM_NO_THREAD_SUPPORT', '-DDLIB_LOG_DOMAIN="TEST"',
                 '-I' + str(work), '-I' + str(work / 'include')]
        flags += ['-I' + str(p / 'src') for p in libraries]
        flags += ['-I' + str(ROOT / 'engine/lua/src/lua')]
        flags += ['-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections']
        sources = ['engine/gamesys/src/gamesys/test/test_mesh_indices_host.cpp',
                   'engine/dlib/src/dlib/buffer.cpp', 'engine/dlib/src/dlib/array.cpp',
                   'engine/dlib/src/dlib/hash.cpp', 'engine/dlib/src/dlib/memory.cpp',
                   'engine/dlib/src/dlib/dstrings.cpp', 'engine/dlib/src/dlib/mutex_web.cpp']
        binary = work / 'test_mesh_indices_host'
        run([os.environ.get('CXX', 'c++')] + flags + [ROOT / p for p in sources] + ['-o', binary])
        run([binary])


if __name__ == '__main__':
    main()
