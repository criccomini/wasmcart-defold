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

// Standalone test: compile this translation unit with a host C++ compiler.
// Including the backend lets each fixture reset its process-lifetime globals
// to model a new cart instance, without adding test hooks to the engine API.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static bool g_FailAllocation = false;
static void* TestCalloc(size_t count, size_t size)
{
    return g_FailAllocation ? 0 : calloc(count, size);
}
#define calloc TestCalloc
#include "../platform_wasmcart_save.cpp"
#undef calloc

using namespace dmPlatform;

// Independent definition of the original on-disk format.
struct LegacyEntry
{
    char name[128];
    uint32_t size;
    uint8_t data[16384];
};
struct LegacyBlock
{
    uint32_t magic, version, count, reserved;
    LegacyEntry entries[8];
};
static_assert(sizeof(LegacyBlock) == 132144, "Legacy fixture layout");

static void NewCart(int64_t size)
{
    free(g_SaveBlock);
    g_SaveBlock = 0;
    g_SaveSize = g_FileCapacity = 0;
    g_Configured = false;
    assert(WasmcartConfigureSave(size));
}

static std::vector<uint8_t> Snapshot()
{
    const uint8_t* bytes = (const uint8_t*)WasmcartGetSaveBlock();
    return std::vector<uint8_t>(bytes, bytes + WasmcartGetSaveBlockSize());
}

static void Restore(const std::vector<uint8_t>& bytes)
{
    // Model a host that permits a smaller save and zero-pads the new region.
    // The current Couchmix runner rejects size mismatches before cart init;
    // it needs a separate change before these migration fixtures reach us.
    uint32_t count = (uint32_t)bytes.size();
    if (count > WasmcartGetSaveBlockSize())
        count = WasmcartGetSaveBlockSize();
    memcpy(WasmcartGetSaveBlock(), bytes.data(), count);
}

static void CheckRead(const char* name, const void* expected, uint32_t size)
{
    uint32_t stored = 0;
    assert(WasmcartSaveRead(name, 0, 0, &stored));
    assert(stored == size);
    std::vector<uint8_t> result(size);
    assert(WasmcartSaveRead(name, result.data(), size, 0));
    assert(!size || memcmp(result.data(), expected, size) == 0);
}

static LegacyBlock LegacyFixture()
{
    LegacyBlock block = {};
    block.magic = 0x56534357;
    block.version = 1;
    block.count = 8;
    for (uint32_t i = 0; i < 8; ++i)
    {
        snprintf(block.entries[i].name, sizeof(block.entries[i].name), "save-%u", i);
        block.entries[i].size = i == 0 ? 0 : 16384 - i;
        memset(block.entries[i].data, (int)(i + 1), block.entries[i].size);
    }
    return block;
}

static void TestLegacy()
{
    LegacyBlock legacy = LegacyFixture();
    const int64_t capacities[] = { 132144, 1048576, 4194304 };
    for (uint32_t c = 0; c < 3; ++c)
    {
        NewCart(capacities[c]);
        memcpy(WasmcartGetSaveBlock(), &legacy, sizeof(legacy));
        for (uint32_t i = 0; i < 8; ++i)
            CheckRead(legacy.entries[i].name, legacy.entries[i].data, legacy.entries[i].size);
        if (c == 0)
            assert(memcmp(WasmcartGetSaveBlock(), &legacy, sizeof(legacy)) == 0);
        else
        {
            assert(((SaveHeader*)g_SaveBlock)->m_Version == 2);
            for (uint32_t i = 0; i < 8; ++i)
            {
                SaveEntry* entry = EntryAt(i, g_FileCapacity);
                for (uint32_t j = entry->m_Size; j < g_FileCapacity; ++j)
                    assert(EntryData(entry)[j] == 0);
            }
        }
        assert(!WasmcartSaveWrite("ninth", "x", 1));
        assert(WasmcartSaveUnlink("save-2"));
        assert(!WasmcartSaveExists("save-2"));
        CheckRead("save-7", legacy.entries[7].data, legacy.entries[7].size);
        assert(WasmcartSaveWrite("ninth", "x", 1));
        assert(!WasmcartSaveUnlink("absent"));
    }

    // Fresh default writes retain the exact version 1 byte layout too.
    NewCart(132144);
    assert(WasmcartSaveWrite("old", "abc", 3));
    LegacyBlock expected = {};
    expected.magic = legacy.magic;
    expected.version = 1;
    expected.count = 1;
    strcpy(expected.entries[0].name, "old");
    expected.entries[0].size = 3;
    memcpy(expected.entries[0].data, "abc", 3);
    assert(memcmp(g_SaveBlock, &expected, sizeof(expected)) == 0);
}

static void TestCapacityAndRoundTrip()
{
    const int64_t sizes[] = { INT64_MIN, 0, 132144, 132145, 1048576, 4194304, INT64_MAX };
    const uint32_t totals[] = { 132144, 132144, 132144, 132145, 1048576, 4194304, 4194304 };
    const uint32_t limits[] = { 16384, 16384, 16384, 16384, 130936, 524152, 524152 };
    for (uint32_t s = 0; s < 7; ++s)
    {
        NewCart(sizes[s]);
        assert(WasmcartGetSaveBlockSize() == totals[s]);
        assert(WasmcartGetSaveMaxFileSize() == limits[s]);
        void* pointer = WasmcartGetSaveBlock();
        assert(WasmcartConfigureSave(4194304));
        assert(WasmcartGetSaveBlock() == pointer);
        assert(WasmcartGetSaveBlockSize() == totals[s]);

        std::vector<uint8_t> payload(limits[s]);
        for (uint32_t j = 0; j < limits[s]; ++j)
            payload[j] = (uint8_t)(j * 17);
        for (uint32_t i = 0; i < 8; ++i)
        {
            char name[32];
            snprintf(name, sizeof(name), "full-%u", i);
            assert(WasmcartSaveWrite(name, payload.data(), limits[s]));
        }
        std::vector<uint8_t> saved = Snapshot();
        assert(!WasmcartSaveWrite("full-0", payload.data(), limits[s] + 1));
        assert(!WasmcartSaveWrite("ninth", "x", 1));
        assert(Snapshot() == saved);
        NewCart(sizes[s]);
        Restore(saved);
        for (uint32_t i = 0; i < 8; ++i)
        {
            char name[32];
            snprintf(name, sizeof(name), "full-%u", i);
            CheckRead(name, payload.data(), limits[s]);
        }
        uint8_t small[3];
        uint32_t stored = 0;
        assert(WasmcartSaveRead("full-0", small, sizeof(small), &stored));
        assert(stored == limits[s] && memcmp(small, payload.data(), sizeof(small)) == 0);
        assert(WasmcartSaveWrite("full-0", "z", 1));
        SaveEntry* entry = EntryAt(0, g_FileCapacity);
        for (uint32_t j = 1; j < g_FileCapacity; ++j)
            assert(EntryData(entry)[j] == 0);
    }

    NewCart(1048576);
    std::vector<uint8_t> ghost(40960, 0x5a);
    assert(WasmcartSaveWrite("ghost", ghost.data(), (uint32_t)ghost.size()));
    std::vector<uint8_t> full(130936, 0xa5);
    for (uint32_t i = 1; i < 8; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "large-%u", i);
        assert(WasmcartSaveWrite(name, full.data(), (uint32_t)full.size()));
    }
    std::vector<uint8_t> saved = Snapshot();
    NewCart(4194304);
    Restore(saved);
    CheckRead("ghost", ghost.data(), (uint32_t)ghost.size());
    for (uint32_t i = 1; i < 8; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "large-%u", i);
        CheckRead(name, full.data(), (uint32_t)full.size());
    }
    assert(((SaveHeader*)g_SaveBlock)->m_FileCapacity == 524152);
    saved = Snapshot();
    NewCart(132144);
    Restore(saved); // Model a host that truncates when the region shrinks.
    std::vector<uint8_t> truncated = Snapshot();
    assert(!WasmcartSaveExists("ghost"));
    assert(!WasmcartSaveWrite("new", "x", 1));
    assert(Snapshot() == truncated); // Refuse to reinterpret larger slots.
}

static void TestInvalidInputs()
{
    NewCart(132144);
    assert(!WasmcartSaveWrite(0, "x", 1));
    assert(!WasmcartSaveWrite("", "x", 1));
    assert(!WasmcartSaveWrite("x", 0, 1));
    assert(!WasmcartSaveRead(0, 0, 0, 0));
    assert(!WasmcartSaveExists(""));
    assert(!WasmcartSaveUnlink(0));
    char name[129];
    memset(name, 'n', 128);
    name[128] = 0;
    assert(!WasmcartSaveWrite(name, "x", 1));
    name[127] = 0;
    assert(WasmcartSaveWrite(name, 0, 0));
    CheckRead(name, 0, 0);
}

static void TestCorruptRestore()
{
    for (uint32_t c = 0; c < 7; ++c)
    {
        NewCart(1048576);
        LegacyBlock fixture = LegacyFixture();
        if (c == 0) fixture.magic = 0;
        if (c == 1) fixture.version = 99;
        if (c == 2) fixture.count = 9;
        if (c == 3) fixture.entries[0].size = UINT32_MAX;
        if (c == 4) memset(fixture.entries[0].name, 'x', 128);
        if (c == 5) { fixture.version = 2; fixture.reserved = UINT32_MAX; }
        if (c == 6) { fixture.version = 2; fixture.reserved = 16385; }
        memcpy(WasmcartGetSaveBlock(), &fixture, sizeof(fixture));
        assert(!WasmcartSaveExists("save-0"));
        assert(((SaveHeader*)g_SaveBlock)->m_Count == 0);
        assert(WasmcartSaveWrite("new", "ok", 2));
        CheckRead("new", "ok", 2);
    }
}

static void TestAllocationFailure()
{
    free(g_SaveBlock);
    g_SaveBlock = 0;
    g_SaveSize = g_FileCapacity = 0;
    g_Configured = false;
    g_FailAllocation = true;
    assert(!WasmcartConfigureSave(1048576));
    assert(!WasmcartGetSaveBlock());
    assert(WasmcartGetSaveBlockSize() == 0);
    assert(WasmcartGetSaveMaxFileSize() == 0);
    assert(!WasmcartSaveWrite("x", "x", 1));
    assert(!WasmcartSaveRead("x", 0, 0, 0));
    assert(!WasmcartSaveUnlink("x"));
    g_FailAllocation = false;
}

int main()
{
    TestLegacy();
    TestCapacityAndRoundTrip();
    TestInvalidInputs();
    TestCorruptRestore();
    TestAllocationFailure();
    puts("wasmcart save tests passed: legacy migration, 40 KiB ghost, 8 full slots, capacity clamps, round trips, corruption, allocation failure");
    return 0;
}
