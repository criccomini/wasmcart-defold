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

// Archive mounting for a wasmcart cartridge.
//
// The generic mount goes through LoadArchiveFromFile, which fopen()s the
// .arci and .arcd. A cart has no filesystem, so both are read through the
// host's asset imports (via dmSys, which sys_wasmcart.cpp backs) into memory
// and handed to WrapArchiveBuffer instead.
//
// The buffers are owned for the lifetime of the mount: WrapArchiveBuffer does
// not copy them, and the resource system reads resource data straight out of
// the .arcd buffer on every load.

#include <stdlib.h>

#include <dlib/log.h>
#include <dlib/memory.h>
#include <dlib/sys.h>

#include "resource.h"
#include "resource_archive.h"
#include "resource_private.h"

namespace dmResource
{
    struct WasmcartMountInfo
    {
        void* m_IndexBuffer;
        void* m_DataBuffer;
    };

    Result MapFile(const char* filename, void*& map, uint32_t& size)
    {
        return RESULT_OK; // Not used
    }

    Result UnmapFile(void*& map, uint32_t size)
    {
        return RESULT_OK; // Not used
    }

    Result MapAsset(const char* name, void*& out_asset, uint32_t& out_size, void*& out_map)
    {
        return RESULT_OK; // Not used
    }

    Result UnmapAsset(void*& asset, uint32_t size)
    {
        return RESULT_OK; // Not used
    }

    Result MountManifest(const char* manifest_filename, void*& out_map, uint32_t& out_size)
    {
        return RESULT_OK; // Not used
    }

    Result UnmountManifest(void *& map, uint32_t size)
    {
        return RESULT_OK; // Not used
    }

    // Reads a whole cart asset into a 16-byte aligned buffer. The archive
    // index is read as structs, so alignment is not optional.
    static Result LoadWholeAsset(const char* path, void** out_buffer, uint32_t* out_size)
    {
        *out_buffer = 0;
        *out_size   = 0;

        uint32_t size = 0;
        if (dmSys::RESULT_OK != dmSys::ResourceSize(path, &size) || size == 0)
        {
            return RESULT_RESOURCE_NOT_FOUND;
        }

        void* buffer = 0;
        if (dmMemory::RESULT_OK != dmMemory::AlignedMalloc(&buffer, 16, size))
        {
            return RESULT_OUT_OF_MEMORY;
        }

        uint32_t nread = 0;
        if (dmSys::RESULT_OK != dmSys::LoadResource(path, buffer, size, &nread) || nread != size)
        {
            dmMemory::AlignedFree(buffer);
            return RESULT_IO_ERROR;
        }

        *out_buffer = buffer;
        *out_size   = size;
        return RESULT_OK;
    }

    Result MountArchiveInternal(const char* index_path, const char* data_path, dmResourceArchive::HArchiveIndexContainer* archive, void** mount_info)
    {
        *mount_info = 0;

        void*    index_buffer = 0;
        uint32_t index_size   = 0;
        Result r = LoadWholeAsset(index_path, &index_buffer, &index_size);
        if (RESULT_OK != r)
        {
            dmLogError("wasmcart: could not read archive index '%s'", index_path);
            return r;
        }

        void*    data_buffer = 0;
        uint32_t data_size   = 0;
        r = LoadWholeAsset(data_path, &data_buffer, &data_size);
        if (RESULT_OK != r)
        {
            dmLogError("wasmcart: could not read archive data '%s'", data_path);
            dmMemory::AlignedFree(index_buffer);
            return r;
        }

        // mem_mapped = true means "this data is readable as memory", which is
        // exactly what these heap buffers are. With false, ReadEntry takes its
        // FILE* path and freads from a handle a cart does not have.
        // UnmountArchiveInternal below frees the buffers; nothing unmaps them.
        dmResourceArchive::Result ar = dmResourceArchive::WrapArchiveBuffer(
            index_buffer, index_size, true,
            data_buffer,  data_size,  true,
            archive);

        if (dmResourceArchive::RESULT_OK != ar)
        {
            dmLogError("wasmcart: WrapArchiveBuffer failed (%d)", (int) ar);
            dmMemory::AlignedFree(index_buffer);
            dmMemory::AlignedFree(data_buffer);
            if (ar == dmResourceArchive::RESULT_VERSION_MISMATCH)
                return RESULT_VERSION_MISMATCH;
            return RESULT_RESOURCE_NOT_FOUND;
        }

        WasmcartMountInfo* info = (WasmcartMountInfo*) malloc(sizeof(WasmcartMountInfo));
        info->m_IndexBuffer = index_buffer;
        info->m_DataBuffer  = data_buffer;
        *mount_info = info;

        return RESULT_OK;
    }

    void UnmountArchiveInternal(dmResourceArchive::HArchiveIndexContainer& archive, void* mount_info)
    {
        dmResourceArchive::Delete(archive);

        WasmcartMountInfo* info = (WasmcartMountInfo*) mount_info;
        if (info)
        {
            dmMemory::AlignedFree(info->m_IndexBuffer);
            dmMemory::AlignedFree(info->m_DataBuffer);
            free(info);
        }
    }
}
