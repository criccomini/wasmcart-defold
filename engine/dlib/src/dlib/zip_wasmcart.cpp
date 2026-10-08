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

// A cart has no filesystem. Live Update's ZIP provider calls dmZip::Open,
// so keep the whole host asset alive until that provider closes the ZIP.

#include "zip_private.h"

#include <stdlib.h>
#include <string.h>

#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_asset_size")))
#endif
extern "C" int wc_asset_size(const char* path, unsigned int path_len);

#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_load_asset")))
#endif
extern "C" int wc_load_asset(const char* path, unsigned int path_len,
                             void* out, unsigned int out_len);

namespace dmZip
{

static void CloseAsset(void* buffer)
{
    free(buffer);
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

    void* buffer = malloc((size_t)size);
    if (!buffer)
        return RESULT_IO_ERROR;

    int nread = wc_load_asset(path, path_len, buffer, (unsigned int)size);
    if (nread != size)
    {
        free(buffer);
        return RESULT_IO_ERROR;
    }

    // The ZIP reader borrows this buffer; it does not copy it. CloseAsset
    // runs after zip_close, including when the resource provider rejects the
    // manifest. Each mount owns its buffer; there is no permanent cache.
    zip_t* archive = zip_stream_open((const char*)buffer, (size_t)size, 9, 'r');
    if (!archive)
    {
        free(buffer);
        return RESULT_IO_ERROR;
    }
    return OpenArchive(archive, buffer, CloseAsset, zip);
}

} // namespace dmZip
