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

// Persistent save storage for wasmcart carts.
//
// A cart is a standalone wasm module with no filesystem: every fopen in
// sys.save fails, which is why sys.save reported "Could not write to the
// file" on this port. What a cart DOES have is the ABI's save block - a
// fixed-size region of its own linear memory that the host persists across
// sessions and restores on load (wc_info_t::save_ptr / save_size).
//
// So this is a tiny fixed-capacity store laid out inside that block, keyed by
// the same path strings sys.get_save_file produces. It is deliberately small
// and flat: a handful of files of a few KB each covers what sys.save is for
// (settings, progress, high scores) and keeps the whole directory scannable
// without an allocator, which matters because the host copies this region
// verbatim and any pointer stored in it would be meaningless on reload.
//
// Layout, all little-endian, written in place so the host's byte copy is the
// entire persistence mechanism:
//
//   magic    u32   'WCSV'  - distinguishes an initialised block from the
//                            zero-fill a first run sees
//   version  u32   1
//   count    u32   number of live entries
//   reserved u32
//   entries[WASMCART_SAVE_MAX_FILES]:
//     name   char[WASMCART_SAVE_MAX_NAME]  NUL-terminated
//     size   u32
//     data   u8[WASMCART_SAVE_MAX_FILE]

#include <stdint.h>
#include <string.h>

#include "platform_window_wasmcart.h"

namespace dmPlatform
{
    static const uint32_t WASMCART_SAVE_MAGIC   = 0x56534357; // 'WCSV' little-endian
    static const uint32_t WASMCART_SAVE_VERSION = 1;

    struct SaveEntry
    {
        char     m_Name[WASMCART_SAVE_MAX_NAME];
        uint32_t m_Size;
        uint8_t  m_Data[WASMCART_SAVE_MAX_FILE];
    };

    struct SaveBlock
    {
        uint32_t  m_Magic;
        uint32_t  m_Version;
        uint32_t  m_Count;
        uint32_t  m_Reserved;
        SaveEntry m_Entries[WASMCART_SAVE_MAX_FILES];
    };

    // Lives in the cart's own linear memory: the shim publishes its address
    // as save_ptr and the host persists exactly sizeof(SaveBlock) bytes.
    static SaveBlock g_SaveBlock;

    void* WasmcartGetSaveBlock()
    {
        return &g_SaveBlock;
    }

    uint32_t WasmcartGetSaveBlockSize()
    {
        return (uint32_t) sizeof(SaveBlock);
    }

    // A host that restored a block from an earlier session hands it back
    // verbatim; a first run hands back zeroes. Both are handled here rather
    // than by a separate "is this the first run" flag, which could disagree
    // with the block's own contents.
    static SaveBlock* GetBlock()
    {
        if (g_SaveBlock.m_Magic != WASMCART_SAVE_MAGIC ||
            g_SaveBlock.m_Version != WASMCART_SAVE_VERSION)
        {
            memset(&g_SaveBlock, 0, sizeof(g_SaveBlock));
            g_SaveBlock.m_Magic   = WASMCART_SAVE_MAGIC;
            g_SaveBlock.m_Version = WASMCART_SAVE_VERSION;
            g_SaveBlock.m_Count   = 0;
        }
        // A corrupt or truncated restore could claim more entries than exist.
        if (g_SaveBlock.m_Count > WASMCART_SAVE_MAX_FILES)
        {
            g_SaveBlock.m_Count = WASMCART_SAVE_MAX_FILES;
        }
        return &g_SaveBlock;
    }

    static SaveEntry* FindEntry(SaveBlock* block, const char* name)
    {
        for (uint32_t i = 0; i < block->m_Count; ++i)
        {
            if (strncmp(block->m_Entries[i].m_Name, name, WASMCART_SAVE_MAX_NAME) == 0)
            {
                return &block->m_Entries[i];
            }
        }
        return 0;
    }

    bool WasmcartSaveWrite(const char* name, const void* data, uint32_t size)
    {
        if (!name || !name[0] || size > WASMCART_SAVE_MAX_FILE)
        {
            return false;
        }
        if (strlen(name) >= WASMCART_SAVE_MAX_NAME)
        {
            return false;
        }

        SaveBlock* block = GetBlock();
        SaveEntry* entry = FindEntry(block, name);
        if (entry == 0)
        {
            if (block->m_Count >= WASMCART_SAVE_MAX_FILES)
            {
                return false;
            }
            entry = &block->m_Entries[block->m_Count++];
            memset(entry, 0, sizeof(*entry));
            strncpy(entry->m_Name, name, WASMCART_SAVE_MAX_NAME - 1);
        }

        // Zero the tail as well as writing the payload: the block is copied
        // whole, so a shorter rewrite that left old bytes behind would ship
        // the previous save's data inside this one's slot.
        memset(entry->m_Data, 0, WASMCART_SAVE_MAX_FILE);
        if (size && data)
        {
            memcpy(entry->m_Data, data, size);
        }
        entry->m_Size = size;
        return true;
    }

    bool WasmcartSaveRead(const char* name, void* data, uint32_t capacity, uint32_t* out_size)
    {
        if (!name || !name[0])
        {
            return false;
        }
        SaveBlock* block = GetBlock();
        SaveEntry* entry = FindEntry(block, name);
        if (entry == 0)
        {
            return false;
        }
        if (out_size)
        {
            *out_size = entry->m_Size;
        }
        if (data && capacity)
        {
            uint32_t n = entry->m_Size < capacity ? entry->m_Size : capacity;
            memcpy(data, entry->m_Data, n);
        }
        return true;
    }

    bool WasmcartSaveExists(const char* name)
    {
        if (!name || !name[0])
        {
            return false;
        }
        return FindEntry(GetBlock(), name) != 0;
    }

    bool WasmcartSaveUnlink(const char* name)
    {
        if (!name || !name[0])
        {
            return false;
        }
        SaveBlock* block = GetBlock();
        for (uint32_t i = 0; i < block->m_Count; ++i)
        {
            if (strncmp(block->m_Entries[i].m_Name, name, WASMCART_SAVE_MAX_NAME) == 0)
            {
                // Swap the last entry into the hole rather than shifting: the
                // directory has no ordering contract and a memmove of the
                // whole tail would copy megabytes to delete one file.
                if (i != block->m_Count - 1)
                {
                    memcpy(&block->m_Entries[i], &block->m_Entries[block->m_Count - 1], sizeof(SaveEntry));
                }
                memset(&block->m_Entries[block->m_Count - 1], 0, sizeof(SaveEntry));
                block->m_Count--;
                return true;
            }
        }
        return false;
    }
}
