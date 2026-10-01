// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// Resource access for a wasmcart cartridge.
//
// A cart has no filesystem. Its assets live inside the .wasc next to the
// wasm module, and the host serves them through two imports: wc_asset_size
// and wc_load_asset, both keyed by path.
//
// Everything above this file -- LoadManifest, the archive provider,
// MountArchiveInternal -- already funnels through dmSys::ResolveMountFileName,
// ResourceExists, ResourceSize and LoadResource. Backing those four with the
// cart imports makes the whole existing resource stack work unchanged, which
// is why there is no wasmcart resource *provider*: there does not need to be
// one.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dlib/dstrings.h>
#include <dlib/log.h>
#include <dlib/path.h>
#include <dlib/sys.h>

// dmSysPosix is compiled for this target (sys_posix.cpp); it just does not
// expose these under dmSys. Declare what we forward to.
namespace dmSysPosix
{
    dmSys::Result GetHostFileName(char* buffer, size_t buffer_size, const char* path);
    dmSys::Result Rename(const char* dst_filename, const char* src_filename);
    dmSys::Result Unlink(const char* path);
    dmSys::Result Stat(const char* path, dmSys::StatInfo* stat_info);
    void FillTimeZone(dmSys::SystemInfo* info);
}

#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_asset_size")))
extern "C" int wc_asset_size(const char* path, unsigned int path_len);

__attribute__((import_module("env"), import_name("wc_load_asset")))
extern "C" int wc_load_asset(const char* path, unsigned int path_len,
                             void* out, unsigned int out_len);
#else
extern "C" int wc_asset_size(const char* path, unsigned int path_len)
{
    (void)path; (void)path_len; return -1;
}
extern "C" int wc_load_asset(const char* path, unsigned int path_len,
                             void* out, unsigned int out_len)
{
    (void)path; (void)path_len; (void)out; (void)out_len; return -1;
}
#endif

namespace dmSys
{
    bool ResourceExists(const char* path);

    // The engine asks for paths like "./game.arci" or "game.dmanifest"; the
    // host keys its asset table on the plain name the packer stored. Strip a
    // leading "./" and any directory prefix the engine prepended for a
    // filesystem that does not exist here.
    static const char* NormalizeAssetPath(const char* path, char* buffer, size_t buffer_size)
    {
        if (!path)
        {
            return 0;
        }

        const char* p = path;
        if (p[0] == '.' && p[1] == '/')
        {
            p += 2;
        }
        // Hosts reject absolute paths outright (path traversal defence), and
        // the engine builds several of these by concatenation, so strip any
        // leading separators rather than handing the host a path it must
        // refuse.
        while (*p == '/' || *p == '\\')
        {
            ++p;
        }

        // A cart's asset names have no directory component, so take the last
        // path segment. An asset the host does stores under a nested name is
        // still reachable because we try the full string first (see below).
        dmStrlCpy(buffer, p, buffer_size);
        return buffer;
    }

    static int AssetSize(const char* path)
    {
        if (!path || !path[0])
        {
            return -1;
        }

        // Try the path as given first, so a host that stores nested names
        // still matches, then fall back to the basename.
        int size = wc_asset_size(path, (unsigned int) strlen(path));
        if (size >= 0)
        {
            return size;
        }

        const char* slash = strrchr(path, '/');
        if (slash && slash[1])
        {
            return wc_asset_size(slash + 1, (unsigned int) strlen(slash + 1));
        }
        return -1;
    }

    static int AssetLoad(const char* path, void* buffer, uint32_t buffer_size)
    {
        if (!path || !path[0])
        {
            return -1;
        }

        int n = wc_load_asset(path, (unsigned int) strlen(path), buffer, buffer_size);
        if (n >= 0)
        {
            return n;
        }

        const char* slash = strrchr(path, '/');
        if (slash && slash[1])
        {
            return wc_load_asset(slash + 1, (unsigned int) strlen(slash + 1), buffer, buffer_size);
        }
        return -1;
    }

    // dmConfigFile gates its load on this before touching the file, so it
    // must see cart assets too, not a filesystem that isn't there.
    bool Exists(const char* path)
    {
        return ResourceExists(path);
    }

    bool ResourceExists(const char* path)
    {
        char norm[DMPATH_MAX_PATH];
        const char* p = NormalizeAssetPath(path, norm, sizeof(norm));
        return p != 0 && AssetSize(p) >= 0;
    }

    Result ResourceSize(const char* path, uint32_t* resource_size)
    {
        char norm[DMPATH_MAX_PATH];
        const char* p = NormalizeAssetPath(path, norm, sizeof(norm));
        if (!p)
        {
            return RESULT_INVAL;
        }

        int size = AssetSize(p);
        if (size < 0)
        {
            return RESULT_NOENT;
        }

        *resource_size = (uint32_t) size;
        return RESULT_OK;
    }

    Result LoadResource(const char* path, void* buffer, uint32_t buffer_size, uint32_t* resource_size)
    {
        *resource_size = 0;

        char norm[DMPATH_MAX_PATH];
        const char* p = NormalizeAssetPath(path, norm, sizeof(norm));
        if (!p)
        {
            return RESULT_INVAL;
        }

        int size = AssetSize(p);
        if (size < 0)
        {
            return RESULT_NOENT;
        }
        if ((uint32_t) size > buffer_size)
        {
            return RESULT_INVAL;
        }

        int nread = AssetLoad(p, buffer, buffer_size);
        if (nread < 0)
        {
            return RESULT_IO;
        }

        *resource_size = (uint32_t) nread;
        return RESULT_OK;
    }

    Result LoadResourcePartial(const char* path, uint32_t offset, uint32_t size, void* buffer, uint32_t* nread)
    {
        *nread = 0;

        if (buffer == 0 || size == 0)
        {
            return RESULT_INVAL;
        }

        char norm[DMPATH_MAX_PATH];
        const char* p = NormalizeAssetPath(path, norm, sizeof(norm));
        if (!p)
        {
            return RESULT_INVAL;
        }

        int total = AssetSize(p);
        if (total < 0)
        {
            return RESULT_NOENT;
        }
        if (offset >= (uint32_t) total)
        {
            return RESULT_OK; // past EOF: zero bytes read, not an error
        }

        // The cart ABI has no seek, so read the whole asset and copy the
        // requested window out of it. Callers use this for streaming reads of
        // the .arcd, so keep one scratch buffer rather than reallocating per
        // call.
        static uint8_t* s_Scratch     = 0;
        static uint32_t s_ScratchSize = 0;
        static char     s_ScratchPath[DMPATH_MAX_PATH] = {0};

        if (s_ScratchSize < (uint32_t) total || strcmp(s_ScratchPath, p) != 0)
        {
            uint8_t* grown = (uint8_t*) realloc(s_Scratch, (size_t) total);
            if (!grown)
            {
                return RESULT_IO;
            }
            s_Scratch     = grown;
            s_ScratchSize = (uint32_t) total;

            int n = AssetLoad(p, s_Scratch, s_ScratchSize);
            if (n < 0)
            {
                s_ScratchPath[0] = 0;
                return RESULT_IO;
            }
            dmStrlCpy(s_ScratchPath, p, sizeof(s_ScratchPath));
        }

        uint32_t avail = (uint32_t) total - offset;
        uint32_t count = size < avail ? size : avail;
        memcpy(buffer, s_Scratch + offset, count);
        *nread = count;
        return RESULT_OK;
    }

    // ---------------------------------------------------------------------
    // The rest of the dmSys surface.
    //
    // sys_web.cpp normally supplies these (mostly by forwarding to
    // dmSysPosix, which is compiled but only exposes them under its own
    // namespace). A cart drops sys_web because it is written against JS glue,
    // so without these the engine links them as wasm IMPORTS -- and with
    // ERROR_ON_UNDEFINED_SYMBOLS=0 that failure is silent until the engine
    // calls one and dereferences whatever the host returned.
    // ---------------------------------------------------------------------

    char* GetEnv(const char* name)
    {
        // A cart has no environment. Forwarding to the posix implementation
        // drags in getenv(), which emscripten backs with the WASI
        // environ_get/environ_sizes_get imports -- two host imports for a
        // lookup that can only ever return null here.
        (void) name;
        return 0;
    }

    Result GetHostFileName(char* buffer, size_t buffer_size, const char* path)
    {
        return dmSysPosix::GetHostFileName(buffer, buffer_size, path);
    }

    Result Rename(const char* dst_filename, const char* src_filename)
    {
        return dmSysPosix::Rename(dst_filename, src_filename);
    }

    Result GetResourcesPath(int argc, char* argv[], char* path, uint32_t path_len)
    {
        // Resources live in the cart, not on a path. An empty string keeps
        // the engine's concatenations ("" + "/game.dmanifest") to bare asset
        // names, which is exactly what the host's asset table is keyed on.
        if (path_len == 0)
        {
            return RESULT_INVAL;
        }
        path[0] = '\0';
        return RESULT_OK;
    }

    Result GetLogPath(char* path, uint32_t path_len)
    {
        if (path_len == 0)
        {
            return RESULT_INVAL;
        }
        path[0] = '\0';
        return RESULT_OK;
    }

    Result Unlink(const char* path)
    {
        return dmSysPosix::Unlink(path);
    }

    Result Stat(const char* path, StatInfo* stat_info)
    {
        return dmSysPosix::Stat(path, stat_info);
    }

    int FileSeek64(FILE* file, uint64_t offset)
    {
        // Reached only if something still holds a FILE*; a cart's archive is
        // wrapped in memory (mount_wasmcart.cpp), so this is defensive.
        return fseek(file, (long) offset, SEEK_SET);
    }

    // Saves are host-managed through the wasmcart save region, so a cart has
    // no writable directory of its own. Empty paths, reported OK, keep the
    // engine from treating this as a fatal condition.
    Result GetApplicationSupportPath(const char* application_name, char* path, uint32_t path_len)
    {
        if (path_len == 0)
        {
            return RESULT_INVAL;
        }
        path[0] = '\0';
        return RESULT_OK;
    }

    Result GetApplicationSavePath(const char* application_name, char* path, uint32_t path_len)
    {
        return GetApplicationSupportPath(application_name, path, path_len);
    }

    Result GetApplicationPath(char* path, uint32_t path_len)
    {
        if (path_len == 0)
        {
            return RESULT_INVAL;
        }
        path[0] = '\0';
        return RESULT_OK;
    }

    void GetSystemInfo(SystemInfo* info)
    {
        memset(info, 0, sizeof(*info));
        dmStrlCpy(info->m_SystemName, "wasmcart", sizeof(info->m_SystemName));
        dmStrlCpy(info->m_DeviceModel, "wasmcart", sizeof(info->m_DeviceModel));
        // No navigator to ask; state a stable default rather than leaving the
        // language fields empty, which some call sites treat as a parse error.
        FillLanguageTerritory("en_US", info);
        dmSysPosix::FillTimeZone(info);
    }

    void GetSecureInfo(SystemInfo* info)
    {
        // Nothing secure to report from inside a sandbox.
    }

    bool GetApplicationInfo(const char* id, ApplicationInfo* info)
    {
        memset(info, 0, sizeof(*info));
        return false; // a cart cannot see other installed applications
    }

    Result OpenURL(const char* url, const char* target)
    {
        // Opening a URL is the host's call, not the cart's.
        return RESULT_UNKNOWN;
    }

    void SetNetworkConnectivityHost(const char* host)
    {
    }

    NetworkConnectivity GetNetworkConnectivity()
    {
        // The peer API is the cart's network surface; report connected so the
        // engine does not disable subsystems that only need "not offline".
        return NETWORK_CONNECTED;
    }

    // DM_SYS_CUSTOM_HOST_PATHS: there is no host filesystem to fall back to,
    // so the asset name is the whole resolution.
    Result ResolveMountFileName(char* buffer, size_t buffer_size, const char* path)
    {
        dmSnPrintf(buffer, buffer_size, "%s", path);
        return ResourceExists(buffer) ? RESULT_OK : RESULT_NOENT;
    }
}
