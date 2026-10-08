"""Compile the production GL mappings for native carts and browser builds.

The small enum headers make this independent of an installed GL SDK. The
native GLES upload behavior is checked by scripts/test_label_glyphs.sh too.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'engine/graphics/src/opengl/graphics_opengl_defines.h'
ENUMS = '''#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190a
#define GL_ES_VERSION_2_0 1
'''

class FontFormats(unittest.TestCase):
    def compile(self, defines, one, two):
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            for p in ['GL/gl.h', 'GL/glext.h', 'GLES2/gl2.h', 'GLES2/gl2ext.h', 'GLES3/gl3.h']:
                target = tmp / p
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(ENUMS)
            source = tmp / 'probe.cpp'
            source.write_text(f'''#include "{HEADER}"
static_assert(DMGRAPHICS_TEXTURE_FORMAT_LUMINANCE == {one}, "one-channel upload enum");
static_assert(DMGRAPHICS_TEXTURE_FORMAT_LUMINANCE_ALPHA == {two}, "two-channel upload enum");
''')
            result = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', '-fsyntax-only',
                                     '-U__linux__', '-U__MACH__', '-I'+name,
                                     *['-D'+d for d in defines], str(source)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_cart_uses_gles3_channels(self):
        self.compile(['__EMSCRIPTEN__', 'DM_PLATFORM_WASMCART', 'GL_ES_VERSION_2_0'], '0x1903', '0x8227')

    def test_browser_keeps_legacy_mapping(self):
        self.compile(['__EMSCRIPTEN__'], '0x1909', '0x190a')

    def test_native_android_keeps_gles_channels(self):
        self.compile(['ANDROID'], '0x1903', '0x8227')

if __name__ == '__main__':
    unittest.main()
