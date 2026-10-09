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

// Host test for the production index validation/upload helpers. Uses real
// dmBuffer storage and a small graphics adapter; no window or engine SDK link.
// The gamesys fixture in test_gamesys.cpp tests properties and render dispatch.
#include "../components/comp_mesh.h"
#undef DM_DECLARE_COMPONENT_TYPE
#define DM_DECLARE_COMPONENT_TYPE(...)
#include "../components/comp_mesh.cpp"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

static uint32_t g_Errors = 0;
static uint32_t g_Uploads = 0;
static uint32_t g_VertexUploads = 0;
static bool g_Support32 = true;
void LogInternal(LogSeverity severity, const char*, const char*, ...)
{
    if (severity == LOG_SEVERITY_ERROR) ++g_Errors;
}

struct TestIndexBuffer { std::vector<uint8_t> m_Bytes; };
struct TestVertexBuffer { std::vector<uint8_t> m_Bytes; };
namespace dmGraphics
{
    HVertexBuffer NewVertexBuffer(HContext, uint32_t, const void*, BufferUsage)
    { return (HVertexBuffer)new TestVertexBuffer; }
    void SetVertexBufferData(HVertexBuffer handle, uint32_t size, const void* data, BufferUsage)
    {
        ((TestVertexBuffer*)handle)->m_Bytes.assign((const uint8_t*)data, (const uint8_t*)data + size);
        ++g_VertexUploads;
    }
    void DeleteVertexBuffer(HVertexBuffer handle) { delete (TestVertexBuffer*)handle; }
    bool IsIndexBufferFormatSupported(HContext, IndexBufferFormat format)
    { return format == INDEXBUFFER_FORMAT_16 || g_Support32; }
    HIndexBuffer NewIndexBuffer(HContext, uint32_t size, const void* data, BufferUsage)
    {
        TestIndexBuffer* result = new TestIndexBuffer;
        result->m_Bytes.assign((const uint8_t*)data, (const uint8_t*)data + size);
        ++g_Uploads;
        return (HIndexBuffer)result;
    }
    void SetIndexBufferData(HIndexBuffer handle, uint32_t size, const void* data, BufferUsage)
    {
        ((TestIndexBuffer*)handle)->m_Bytes.assign((const uint8_t*)data, (const uint8_t*)data + size);
        ++g_Uploads;
    }
}

static dmBuffer::HBuffer MakeBuffer(dmBuffer::ValueType type, uint8_t components, uint32_t streams = 1, uint32_t count = 6)
{
    dmBuffer::StreamDeclaration declarations[] = {
        {dmHashString64("index"), type, components},
        {dmHashString64("other"), type, components}
    };
    dmBuffer::HBuffer buffer;
    assert(dmBuffer::Create(count, declarations, streams, &buffer) == dmBuffer::RESULT_OK);
    return buffer;
}

int main()
{
    using namespace dmGameSystem;
    dmBuffer::NewContext();
    MeshWorld world;
    world.m_GraphicsContext = 0;
    MeshComponent component = {};
    BufferResource indices = {}, vertices = {};
    vertices.m_ElementCount = 4;
    indices.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_UINT16, 1);
    component.m_BufferResource = &vertices;
    component.m_IndicesResource = &indices;
    component.m_IndicesDirty = true;
    uint16_t* stream;
    uint32_t count, components, stride;
    assert(dmBuffer::GetStream(indices.m_Buffer, dmHashString64("index"), (void**)&stream, &count, &components, &stride) == dmBuffer::RESULT_OK);
    const uint16_t expected[] = {0,1,2,0,2,3};
    memcpy(stream, expected, sizeof(expected));
    UpdateIndexBuffer(&world, &component);
    assert(component.m_IndicesValid && component.m_IndexCount == 6);
    assert(component.m_IndexType == dmGraphics::TYPE_UNSIGNED_SHORT && g_Uploads == 1);
    TestIndexBuffer* gpu = (TestIndexBuffer*)component.m_IndexBuffer;
    assert(gpu->m_Bytes.size() == sizeof(expected));
    assert(memcmp(gpu->m_Bytes.data(), expected, sizeof(expected)) == 0);
    UpdateIndexBuffer(&world, &component);
    assert(g_Uploads == 1); // Unchanged data does not upload every frame.
    stream[0] = 3;
    dmBuffer::UpdateContentVersion(indices.m_Buffer);
    UpdateIndexBuffer(&world, &component);
    assert(g_Uploads == 2 && component.m_IndicesValid);
    assert(((uint16_t*)gpu->m_Bytes.data())[0] == 3);
    vertices.m_ElementCount = 3;
    UpdateIndexBuffer(&world, &component);
    assert(!component.m_IndicesValid && g_Uploads == 2);
    vertices.m_ElementCount = 4;
    UpdateIndexBuffer(&world, &component);
    assert(component.m_IndicesValid && g_Uploads == 3);
    stream[0] = 4;
    dmBuffer::UpdateContentVersion(indices.m_Buffer);
    UpdateIndexBuffer(&world, &component);
    assert(!component.m_IndicesValid && g_Uploads == 3);

    dmBuffer::HBuffer previous = indices.m_Buffer;
    indices.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_UINT32, 1, 1, 3);
    // Deliberately leave BufferResource's cached count/stride stale. The index
    // path must read the live buffer, including its changed layout and handle.
    uint32_t* stream32;
    assert(dmBuffer::GetStream(indices.m_Buffer, dmHashString64("index"), (void**)&stream32, &count, &components, &stride) == dmBuffer::RESULT_OK);
    stream32[0] = 0; stream32[1] = 2; stream32[2] = 3;
    dmBuffer::Destroy(previous);
    UpdateIndexBuffer(&world, &component);
    assert(component.m_IndicesValid && component.m_IndexCount == 3);
    assert(component.m_IndexType == dmGraphics::TYPE_UNSIGNED_INT && g_Uploads == 4);
    assert(gpu->m_Bytes.size() == 12 && ((uint32_t*)gpu->m_Bytes.data())[2] == 3);
    g_Support32 = false;
    component.m_IndicesDirty = true;
    UpdateIndexBuffer(&world, &component);
    assert(!component.m_IndicesValid && g_Uploads == 4);
    g_Support32 = true;
    dmBuffer::Destroy(indices.m_Buffer);

    const dmBuffer::ValueType invalid_types[] = {dmBuffer::VALUE_TYPE_FLOAT32, dmBuffer::VALUE_TYPE_INT16, dmBuffer::VALUE_TYPE_UINT8};
    for (unsigned i = 0; i < sizeof(invalid_types)/sizeof(invalid_types[0]); ++i)
    {
        indices.m_Buffer = MakeBuffer(invalid_types[i], 1);
        component.m_IndicesDirty = true;
        UpdateIndexBuffer(&world, &component);
        assert(!component.m_IndicesValid && g_Uploads == 4);
        dmBuffer::Destroy(indices.m_Buffer);
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        indices.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_UINT16, i == 0 ? 2 : 1, i == 0 ? 1 : 2);
        component.m_IndicesDirty = true;
        UpdateIndexBuffer(&world, &component);
        assert(!component.m_IndicesValid && g_Uploads == 4);
        dmBuffer::Destroy(indices.m_Buffer);
    }
    indices.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_UINT16, 1, 1, 0);
    component.m_IndicesDirty = true;
    UpdateIndexBuffer(&world, &component);
    assert(component.m_IndicesValid && component.m_IndexCount == 0 && g_Uploads == 4);
    dmBuffer::Destroy(indices.m_Buffer);
    delete gpu;
    // Shared vertex ownership survives vertex/resource replacement and local /
    // world transitions. Unchanged buffers still upload only once.
    BufferResource va = {}, vb = {};
    va.m_NameHash = dmHashString64("vertices-a");
    vb.m_NameHash = dmHashString64("vertices-b");
    va.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_FLOAT32, 3, 1, 4);
    vb.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_FLOAT32, 3, 1, 3);
    va.m_ElementCount = 4; vb.m_ElementCount = 3;
    va.m_Stride = vb.m_Stride = 12;
    MeshComponent a = {}, b = {};
    a.m_BufferResource = b.m_BufferResource = &va;
    SyncLocalVertexBuffer(&world, &a);
    SyncLocalVertexBuffer(&world, &b);
    assert(GetVertexBufferInfo(&world, va.m_NameHash)->m_RefCount == 2 && g_VertexUploads == 1);
    a.m_BufferResource = &vb;
    SyncLocalVertexBuffer(&world, &a);
    assert(GetVertexBufferInfo(&world, va.m_NameHash)->m_RefCount == 1);
    assert(GetVertexBufferInfo(&world, vb.m_NameHash)->m_RefCount == 1 && g_VertexUploads == 2);
    dmBuffer::HBuffer old_vertices = va.m_Buffer;
    va.m_Buffer = MakeBuffer(dmBuffer::VALUE_TYPE_FLOAT32, 3, 1, 4);
    dmBuffer::Destroy(old_vertices);
    SyncLocalVertexBuffer(&world, &b);
    assert(g_VertexUploads == 3); // Same resource, new handle.
    ReleaseLocalVertexBuffer(&world, &a);
    assert(!GetVertexBufferInfo(&world, vb.m_NameHash));
    a.m_BufferResource = &va;
    SyncLocalVertexBuffer(&world, &a);
    assert(GetVertexBufferInfo(&world, va.m_NameHash)->m_RefCount == 2 && g_VertexUploads == 3);
    ReleaseLocalVertexBuffer(&world, &a);
    ReleaseLocalVertexBuffer(&world, &b);
    assert(world.m_ResourceToVertexBuffer.Size() == 0 && world.m_VertexBufferPool.Size() == 2);
    for (uint32_t i = 0; i < world.m_VertexBufferPool.Size(); ++i)
        dmGraphics::DeleteVertexBuffer(world.m_VertexBufferPool[i]);
    dmBuffer::Destroy(va.m_Buffer);
    dmBuffer::Destroy(vb.m_Buffer);
    dmBuffer::DeleteContext();
    assert(g_Errors == 8);
    printf("mesh indices: validation, bounds, versions, replacement, uint16/uint32, empty buffers and vertex ownership passed (4 index uploads, 3 vertex uploads, 8 expected errors)\n");
}
