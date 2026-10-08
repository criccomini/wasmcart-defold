// Native GLES3 coverage for the cart's font atlas upload enums.
// Runs on software EGL. It uses the production header, not copied mappings.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "../opengl/graphics_opengl_defines.h"

static void CheckAtlas(GLenum format, GLenum internal_format, unsigned channels)
{
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    unsigned char zero[4 * 4 * 2] = {};
    // The native host normalizes legacy formats for full uploads. It does
    // not normalize subuploads, so exercise the engine's enum there.
    GLenum full_upload_format = channels == 1 ? GL_RED : GL_RG;
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, 4, 4, 0, full_upload_format, GL_UNSIGNED_BYTE, zero);
    assert(glGetError() == GL_NO_ERROR);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    for (unsigned frame = 1; frame <= 100; ++frame)
    {
        const unsigned char glyph[2] = { (unsigned char)frame, 173 };
        glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, format, GL_UNSIGNED_BYTE, glyph);
        assert(glGetError() == GL_NO_ERROR);
        unsigned char pixels[4 * 4 * 4];
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        assert(glGetError() == GL_NO_ERROR);
        for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
        {
            unsigned offset = (y * 4 + x) * 4;
            bool changed = x == 1 && y == 1;
            assert(pixels[offset] == (changed ? frame : 0));
            assert(pixels[offset + 1] == (changed && channels == 2 ? 173 : 0));
        }
    }
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    assert(display != EGL_NO_DISPLAY);
    assert(eglInitialize(display, 0, 0));
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                            EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig config;
    EGLint count;
    assert(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    assert(context != EGL_NO_CONTEXT);
    assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
    printf("GLES renderer: %s\n", glGetString(GL_RENDERER));
    CheckAtlas(DMGRAPHICS_TEXTURE_FORMAT_LUMINANCE, GL_R8, 1);
    CheckAtlas(DMGRAPHICS_TEXTURE_FORMAT_LUMINANCE_ALPHA, GL_RG8, 2);
    // This was the cart's old glyph subupload. Native GLES3 rejects it.
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    unsigned char zero[16] = {};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 4, 4, 0, GL_RED, GL_UNSIGNED_BYTE, zero);
    assert(glGetError() == GL_NO_ERROR);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, zero);
    GLenum legacy_error = glGetError();
    printf("Legacy glyph subupload error: 0x%x\n", legacy_error);
    assert(legacy_error == GL_INVALID_ENUM || legacy_error == GL_INVALID_OPERATION);
    glDeleteTextures(1, &texture);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    puts("100 one-channel and 100 two-channel atlas updates passed; legacy upload rejected");
}
