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

// A cart has no filesystem. Live Update's ZIP provider calls dmZip::Open;
// retain only an owned asset path and size for its bounded read callback.

#include "zip_private.h"

#include <stdlib.h>
#include <string.h>

#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_asset_size")))
#endif
extern "C" int wc_asset_size(const char* path, unsigned int path_len);

#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_read_asset_range")))
#endif
extern "C" int wc_read_asset_range(const char* path, unsigned int path_len,
                                   unsigned int offset, void* out,
                                   unsigned int out_len);

namespace dmZip
{

struct AssetContext
{
    unsigned int m_PathLength;
    unsigned int m_Size;
    char         m_Path[1];
};

static void CloseAsset(void* context)
{
    free(context);
}

static size_t ReadAssetAt(FILE* stream, uint64_t offset, void* buffer, size_t size)
{
    AssetContext* asset = (AssetContext*)stream;
    if (!asset || !size || !buffer || offset > asset->m_Size ||
        size > asset->m_Size - offset)
        return 0;

    // wc_asset_size returns a nonnegative int, so the bounds above also keep
    // offset and size representable by the range import's unsigned arguments.
    int nread = wc_read_asset_range(asset->m_Path, asset->m_PathLength,
                                    (unsigned int)offset, buffer,
                                    (unsigned int)size);
    if (nread < 0 || (size_t)nread > size)
        return 0;
    return (size_t)nread;
}

Result OpenFileRange(FILE*, uint64_t, uint64_t, HZip* zip)
{
    *zip = 0;
    return RESULT_IO_ERROR; // A cart cannot read a FILE range.
}

Result OpenPlatform(const char* path, HZip* zip)
{
    *zip = 0;
    // dmZip::Open has normalized separators. Match the cart resource paths,
    // but keep directories: parts/area1.zip and parts/area2.zip are distinct.
    while (path[0] == '.' && path[1] == '/')
        path += 2;
    while (*path == '/')
        ++path;

    unsigned int path_len = (unsigned int)strlen(path);
    int size = wc_asset_size(path, path_len);
    if (size < 0)
        return RESULT_NO_SUCH_ENTRY;
    if (size == 0)
        return RESULT_IO_ERROR;

    AssetContext* asset = (AssetContext*)malloc(sizeof(AssetContext) + path_len);
    if (!asset)
        return RESULT_IO_ERROR;
    asset->m_PathLength = path_len;
    asset->m_Size = (unsigned int)size;
    memcpy(asset->m_Path, path, (size_t)path_len + 1);

    // The existing bounded ZIP reader treats this FILE* as opaque and calls
    // only ReadAssetAt; it never performs FILE operations on the context.
    // OpenArchive releases it on open failure, or after zip_close on unmount.
    // Range reads are synchronous and require a STORED outer cart asset.
    // Inner ZIP extraction/CRC checks are unchanged; range reads do not verify
    // the outer entry CRC (wc_load_asset remains its full-asset verifier).
    Result result = OpenFileRangeInternal((FILE*)asset, 0, (uint64_t)size,
                                          ReadAssetAt, asset, CloseAsset, zip);
    return result == RESULT_NO_SUCH_ENTRY ? RESULT_IO_ERROR : result;
}

} // namespace dmZip
