// Copyright 2020-2026 The Defold Foundation
// Licensed under the Defold License version 1.0. See LICENSE.txt.
// Standalone test: real dmZip + ZIP reader, mocked wasmcart host imports.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include <dlib/zip_private.h>
#include <dlib/uri.h>

static std::map<std::string, std::vector<char> > assets;
static std::map<void*, size_t> buffers;
static unsigned loads;
static bool fail_load;
static bool short_load;
static bool fail_alloc;

static void* AssetMalloc(size_t size)
{
    if (fail_alloc)
        return 0;
    void* buffer = malloc(size);
    assert(buffer);
    buffers[buffer] = size;
    return buffer;
}

static void AssetFree(void* buffer)
{
    assert(buffers.erase(buffer) == 1);
    free(buffer);
}

// Count only the platform backend's owned asset buffers. The real ZIP reader
// is compiled separately, so its allocations still use the normal allocator.
#define malloc AssetMalloc
#define free AssetFree
#include "../dlib/zip_wasmcart.cpp"
#undef malloc
#undef free

// Exercise Live Update's real add/remove code without building the engine.
// Only provider registration and the mount table are mocked; ZIP loading and
// cleanup still use the cart backend above.
#include <dmsdk/extension/extension.hpp>
#undef DM_DECLARE_EXTENSION
#define DM_DECLARE_EXTENSION(...)
#include "../../../liveupdate/src/liveupdate.cpp"

void LogInternal(LogSeverity, const char*, const char*, ...) {}
uint64_t dmHashString64(const char*) { return 1; }
const char* dmHashReverseSafe64(uint64_t) { return "test"; }

namespace dmResource
{
const char* ResultToString(Result) { return "test"; }
}

namespace dmResourceProvider
{
struct Archive { dmZip::HZip m_Zip; };
static bool registered = true;
HArchiveLoader FindLoaderByName(dmhash_t)
{
    return registered ? (HArchiveLoader)1 : 0;
}
Result CreateMount(HArchiveLoader, const dmURI::Parts* uri, HArchive, HArchive* out)
{
    dmZip::HZip zip;
    if (dmZip::Open(uri->m_Path, &zip) != dmZip::RESULT_OK)
        return RESULT_IO_ERROR;
    *out = new Archive{zip};
    return RESULT_OK;
}
Result Unmount(HArchive archive)
{
    dmZip::Close(archive->m_Zip);
    delete archive;
    return RESULT_OK;
}
}

namespace dmResourceMounts
{
static std::map<dmhash_t, dmResourceProvider::HArchive> mounts;
dmMutex::HMutex GetMutex(HContext) { return 0; }
dmResource::Result AddMount(HContext, dmhash_t name, dmResourceProvider::HArchive archive, int)
{
    if (mounts.count(name))
        return dmResource::RESULT_INVAL;
    mounts[name] = archive;
    return dmResource::RESULT_OK;
}
dmResource::Result RemoveAndUnmountByNameHash(HContext, dmhash_t name)
{
    auto entry = mounts.find(name);
    if (entry == mounts.end())
        return dmResource::RESULT_RESOURCE_NOT_FOUND;
    dmResourceProvider::Unmount(entry->second);
    mounts.erase(entry);
    return dmResource::RESULT_OK;
}
}

extern "C" int wc_asset_size(const char* path, unsigned int length)
{
    auto entry = assets.find(std::string(path, length));
    return entry == assets.end() ? -1 : (int)entry->second.size();
}

extern "C" int wc_load_asset(const char* path, unsigned int length,
                              void* out, unsigned int capacity)
{
    ++loads;
    if (fail_load)
        return -1;
    const auto& data = assets.at(std::string(path, length));
    assert(capacity == data.size());
    unsigned count = capacity - (short_load ? 1 : 0);
    memcpy(out, data.data(), count);
    return (int)count;
}

static std::vector<char> ReadFixture(const char* path)
{
    FILE* file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0);
    rewind(file);
    std::vector<char> data((size_t)size);
    assert(fread(data.data(), 1, data.size(), file) == data.size());
    fclose(file);
    return data;
}

static void ReadHello(dmZip::HZip zip)
{
    assert(dmZip::GetNumEntries(zip) == 4);
    assert(dmZip::OpenEntry(zip, "hello.txt") == dmZip::RESULT_OK);
    uint32_t size = 0;
    assert(dmZip::GetEntrySize(zip, &size) == dmZip::RESULT_OK && size == 10);
    char data[10];
    assert(dmZip::GetEntryData(zip, data, sizeof(data)) == dmZip::RESULT_OK);
    assert(memcmp(data, "Hello Zip\n", sizeof(data)) == 0);
    assert(dmZip::CloseEntry(zip) == dmZip::RESULT_OK);
    assert(dmZip::OpenEntry(zip, "dir/data.bin") == dmZip::RESULT_OK);
    uint32_t read = 0;
    assert(dmZip::GetEntryDataOffset(zip, 6, 2, data, &read) == dmZip::RESULT_OK);
    assert(read == 2 && data[0] == 6 && data[1] == 7);
    dmZip::CloseEntry(zip);
}

static void Reject(const char* path, dmZip::Result expected)
{
    dmZip::HZip zip = (dmZip::HZip)1;
    assert(dmZip::Open(path, &zip) == expected);
    assert(zip == 0 && buffers.empty());
}

int main()
{
    assets["parts/area1.zip"] = ReadFixture("engine/dlib/src/test/data/zip/archive_deflated.zip");
    assets["parts/area2.zip"] = ReadFixture("engine/dlib/src/test/data/zip/archive_stored.zip");
    assets["broken.zip"] = std::vector<char>(100, 'x');
    assets["empty.zip"] = {};
    Reject("absent.zip", dmZip::RESULT_NO_SUCH_ENTRY);
    Reject("empty.zip", dmZip::RESULT_IO_ERROR);
    Reject("broken.zip", dmZip::RESULT_IO_ERROR);
    fail_load = true;
    Reject("parts/area1.zip", dmZip::RESULT_IO_ERROR);
    fail_load = false;
    short_load = true;
    Reject("parts/area1.zip", dmZip::RESULT_IO_ERROR);
    short_load = false;
    fail_alloc = true;
    Reject("parts/area1.zip", dmZip::RESULT_IO_ERROR);
    fail_alloc = false;
    auto truncated = assets["parts/area1.zip"];
    truncated.pop_back();
    assets["truncated.zip"] = truncated;
    Reject("truncated.zip", dmZip::RESULT_IO_ERROR);

    // Exercise the URI/path boundary used by the ZIP resource provider.
    dmURI::Parts uri;
    assert(dmURI::Parse("zip:parts/area1.zip", &uri) == dmURI::RESULT_OK);
    assert(strcmp(uri.m_Scheme, "zip") == 0);
    for (unsigned i = 0; i < 100; ++i)
    {
        unsigned before = loads;
        dmZip::HZip first, second;
        assert(dmZip::Open(uri.m_Path, &first) == dmZip::RESULT_OK);
        assert(dmZip::Open("./parts\\area2.zip", &second) == dmZip::RESULT_OK);
        assert(buffers.size() == 2 && loads == before + 2);
        size_t bytes = 0;
        for (const auto& buffer : buffers)
            bytes += buffer.second;
        assert(bytes == assets["parts/area1.zip"].size() + assets["parts/area2.zip"].size());
        ReadHello(first);
        ReadHello(second);
        assert(loads == before + 2); // Entry reads never reload the host asset.
        dmZip::Close(first);
        assert(buffers.size() == 1);
        ReadHello(second); // Closing one mount leaves the other readable.
        dmZip::Close(second);
        assert(buffers.empty());
    }
    // Real bob Live Update fixture: inspect its manifest using the same reader.
    assets["parts/live.zip"] = ReadFixture("engine/liveupdate/src/test/data/defold.resourcepack.zip");
    dmZip::HZip live;
    assert(dmZip::Open("parts/live.zip", &live) == dmZip::RESULT_OK);
    assert(dmZip::OpenEntry(live, "liveupdate.game.dmanifest") == dmZip::RESULT_OK);
    uint32_t size;
    assert(dmZip::GetEntrySize(live, &size) == dmZip::RESULT_OK && size > 0);
    std::vector<char> manifest(size);
    assert(dmZip::GetEntryData(live, manifest.data(), size) == dmZip::RESULT_OK);
    dmZip::CloseEntry(live);
    dmZip::Close(live);
    assert(buffers.empty());

    dmLiveUpdate::g_LiveUpdate.m_IsEnabled = true;
    dmLiveUpdate::AddMountInfo job;
    job.m_NameHash = 10;
    job.m_Uri = "zip:parts/area1.zip";
    job.m_Priority = 10;
    for (unsigned i = 0; i < 100; ++i)
    {
        assert(dmLiveUpdate::AddMountProcess(0, 0, &dmLiveUpdate::g_LiveUpdate, &job) == dmLiveUpdate::RESULT_OK);
        assert(buffers.size() == 1);
        // The duplicate must report failure and release only its new ZIP.
        assert(dmLiveUpdate::AddMountProcess(0, 0, &dmLiveUpdate::g_LiveUpdate, &job) != dmLiveUpdate::RESULT_OK);
        assert(buffers.size() == 1 && dmResourceMounts::mounts.size() == 1);
        ReadHello(dmResourceMounts::mounts.at(job.m_NameHash)->m_Zip);
        assert(dmLiveUpdate::RemoveMountSync(job.m_NameHash) == dmLiveUpdate::RESULT_OK);
        assert(buffers.empty() && dmResourceMounts::mounts.empty());
    }
    assert(dmLiveUpdate::RemoveMountSync(job.m_NameHash) != dmLiveUpdate::RESULT_OK);
    job.m_Uri = "zip:broken.zip";
    assert(dmLiveUpdate::AddMountProcess(0, 0, &dmLiveUpdate::g_LiveUpdate, &job) != dmLiveUpdate::RESULT_OK);
    assert(buffers.empty());
    dmResourceProvider::registered = false;
    assert(dmLiveUpdate::AddMountProcess(0, 0, &dmLiveUpdate::g_LiveUpdate, &job) != dmLiveUpdate::RESULT_OK);
    assert(buffers.empty());
    printf("ZIP cart test passed: 100 two-mount cycles, zero retained asset buffers\n");
    printf("Live Update test passed: 100 add/duplicate/remove cycles\n");
    return 0;
}
