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

// Persistent save storage for wasmcart carts. The host restores the byte
// region before wc_init and persists it verbatim. No pointers live in it.
//
// Little-endian layout (unchanged for the default capacity):
//   magic u32 'WCSV', version u32, count u32, file_capacity u32
//   eight slots: name char[128], size u32, data u8[file_capacity]
// Version 1 has a reserved zero in place of file_capacity and 16 KiB slots.
// Version 2 records the capacity. Larger configurations migrate version 1
// in place on first access, after the host has restored it.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform_window_wasmcart.h"

namespace dmPlatform
{
    static const uint32_t WASMCART_SAVE_MAGIC = 0x56534357; // 'WCSV'

    struct SaveHeader
    {
        uint32_t m_Magic;
        uint32_t m_Version;
        uint32_t m_Count;
        uint32_t m_FileCapacity;
    };

    struct SaveEntry
    {
        char     m_Name[WASMCART_SAVE_MAX_NAME];
        uint32_t m_Size;
        // Payload follows immediately, with no pointer in the saved bytes.
    };

    static_assert(sizeof(SaveHeader) == 16, "Save header layout changed");
    static_assert(sizeof(SaveEntry) == 132, "Save entry layout changed");
    static_assert(WASMCART_SAVE_DEFAULT_SIZE == 132144, "Legacy save size changed");

    static uint8_t* g_SaveBlock = 0;
    static uint32_t g_SaveSize = 0;
    static uint32_t g_FileCapacity = 0;
    static bool g_Configured = false;

    bool WasmcartConfigureSave(int64_t size)
    {
        if (g_Configured)
            return g_SaveBlock != 0;
        g_Configured = true;
        if (size < WASMCART_SAVE_DEFAULT_SIZE)
            size = WASMCART_SAVE_DEFAULT_SIZE;
        if (size > WASMCART_SAVE_MAX_SIZE)
            size = WASMCART_SAVE_MAX_SIZE;

        // Keep slot headers aligned to four bytes. Up to 31 trailing bytes
        // remain unused; the total published to the host is the chosen size.
        g_FileCapacity = (((uint32_t)size - sizeof(SaveHeader)) / WASMCART_SAVE_MAX_FILES - sizeof(SaveEntry)) & ~3u;
        g_SaveBlock = (uint8_t*)calloc(1, (size_t)size);
        if (!g_SaveBlock)
        {
            g_FileCapacity = 0;
            return false;
        }
        g_SaveSize = (uint32_t)size;
        return true;
    }

    void* WasmcartGetSaveBlock()
    {
        WasmcartConfigureSave(WASMCART_SAVE_DEFAULT_SIZE);
        return g_SaveBlock;
    }

    uint32_t WasmcartGetSaveBlockSize()
    {
        WasmcartGetSaveBlock();
        return g_SaveSize;
    }

    uint32_t WasmcartGetSaveMaxFileSize()
    {
        WasmcartGetSaveBlock();
        return g_FileCapacity;
    }

    static SaveEntry* EntryAt(uint32_t index, uint32_t capacity)
    {
        return (SaveEntry*)(g_SaveBlock + sizeof(SaveHeader) + index * (sizeof(SaveEntry) + capacity));
    }

    static uint8_t* EntryData(SaveEntry* entry)
    {
        return (uint8_t*)entry + sizeof(SaveEntry);
    }

    static SaveHeader* ResetBlock()
    {
        memset(g_SaveBlock, 0, g_SaveSize);
        SaveHeader* block = (SaveHeader*)g_SaveBlock;
        block->m_Magic = WASMCART_SAVE_MAGIC;
        block->m_Version = g_FileCapacity == WASMCART_SAVE_DEFAULT_FILE ? 1 : 2;
        block->m_FileCapacity = block->m_Version == 1 ? 0 : g_FileCapacity;
        return block;
    }

    static SaveHeader* GetBlock()
    {
        if (!WasmcartGetSaveBlock())
            return 0;
        SaveHeader* block = (SaveHeader*)g_SaveBlock;
        if (block->m_Magic != WASMCART_SAVE_MAGIC ||
            (block->m_Version != 1 && block->m_Version != 2))
            return ResetBlock();

        uint32_t old_capacity = block->m_Version == 1 ? WASMCART_SAVE_DEFAULT_FILE : block->m_FileCapacity;
        const uint32_t max_capacity = ((WASMCART_SAVE_MAX_SIZE - sizeof(SaveHeader)) / WASMCART_SAVE_MAX_FILES - sizeof(SaveEntry)) & ~3u;
        if (block->m_Count > WASMCART_SAVE_MAX_FILES || old_capacity < WASMCART_SAVE_DEFAULT_FILE ||
            old_capacity > max_capacity || (old_capacity & 3u))
            return ResetBlock();

        // A smaller configuration cannot safely restore a larger layout.
        // Refuse access rather than write new entries over the restored data.
        if (old_capacity > g_FileCapacity)
            return 0;

        // Validate before moving anything, including sizes used by sys.load
        // to allocate a deserialization buffer. Never copy beyond a slot.
        for (uint32_t i = 0; i < block->m_Count; ++i)
        {
            SaveEntry* entry = EntryAt(i, old_capacity);
            if (!entry->m_Name[0] || !memchr(entry->m_Name, 0, sizeof(entry->m_Name)) || entry->m_Size > old_capacity)
                return ResetBlock();
        }

        if (old_capacity != g_FileCapacity)
        {
            // Work backwards so expanding a slot cannot overwrite the next
            // old entry before it is moved. No second block is allocated.
            for (uint32_t i = block->m_Count; i > 0; --i)
            {
                SaveEntry* src = EntryAt(i - 1, old_capacity);
                SaveEntry* dst = EntryAt(i - 1, g_FileCapacity);
                uint32_t size = src->m_Size;
                memmove(dst, src, sizeof(SaveEntry) + size);
                memset(EntryData(dst) + size, 0, g_FileCapacity - size);
            }
            uint32_t used = sizeof(SaveHeader) + block->m_Count * (sizeof(SaveEntry) + g_FileCapacity);
            memset(g_SaveBlock + used, 0, g_SaveSize - used);
            block->m_Version = 2;
            block->m_FileCapacity = g_FileCapacity;
        }
        return block;
    }

    static SaveEntry* FindEntry(SaveHeader* block, const char* name)
    {
        if (!block)
            return 0;
        for (uint32_t i = 0; i < block->m_Count; ++i)
        {
            SaveEntry* entry = EntryAt(i, g_FileCapacity);
            if (strncmp(entry->m_Name, name, WASMCART_SAVE_MAX_NAME) == 0)
                return entry;
        }
        return 0;
    }

    bool WasmcartSaveWrite(const char* name, const void* data, uint32_t size)
    {
        if (!name || !name[0] || (size && !data) || size > WasmcartGetSaveMaxFileSize() ||
            strlen(name) >= WASMCART_SAVE_MAX_NAME)
            return false;

        SaveHeader* block = GetBlock();
        if (!block)
            return false;
        SaveEntry* entry = FindEntry(block, name);
        if (!entry)
        {
            if (block->m_Count >= WASMCART_SAVE_MAX_FILES)
                return false;
            entry = EntryAt(block->m_Count++, g_FileCapacity);
            memset(entry, 0, sizeof(SaveEntry));
            strncpy(entry->m_Name, name, WASMCART_SAVE_MAX_NAME - 1);
        }

        // The host persists the whole slot, so erase the previous payload.
        memset(EntryData(entry), 0, g_FileCapacity);
        if (size)
            memcpy(EntryData(entry), data, size);
        entry->m_Size = size;
        return true;
    }

    bool WasmcartSaveRead(const char* name, void* data, uint32_t capacity, uint32_t* out_size)
    {
        if (!name || !name[0])
            return false;
        SaveEntry* entry = FindEntry(GetBlock(), name);
        if (!entry)
            return false;
        if (out_size)
            *out_size = entry->m_Size;
        if (data && capacity)
        {
            uint32_t n = entry->m_Size < capacity ? entry->m_Size : capacity;
            memcpy(data, EntryData(entry), n);
        }
        return true;
    }

    bool WasmcartSaveExists(const char* name)
    {
        if (!name || !name[0])
            return false;
        return FindEntry(GetBlock(), name) != 0;
    }

    bool WasmcartSaveUnlink(const char* name)
    {
        if (!name || !name[0])
            return false;
        SaveHeader* block = GetBlock();
        if (!block)
            return false;
        for (uint32_t i = 0; i < block->m_Count; ++i)
        {
            SaveEntry* entry = EntryAt(i, g_FileCapacity);
            if (strncmp(entry->m_Name, name, WASMCART_SAVE_MAX_NAME) == 0)
            {
                SaveEntry* last = EntryAt(block->m_Count - 1, g_FileCapacity);
                if (entry != last)
                    memcpy(entry, last, sizeof(SaveEntry) + g_FileCapacity);
                memset(last, 0, sizeof(SaveEntry) + g_FileCapacity);
                block->m_Count--;
                return true;
            }
        }
        return false;
    }
}
