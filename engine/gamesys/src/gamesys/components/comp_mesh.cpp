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

#include "comp_mesh.h"

#include <string.h>
#include <float.h>
#include <stdint.h>

#include <dlib/array.h>
#include <dlib/buffer.h>
#include <dlib/hash.h>
#include <dlib/log.h>
#include <dlib/math.h>
#include <dlib/message.h>
#include <dlib/object_pool.h>
#include <dlib/profile.h>
#include <dlib/dstrings.h>
#include <dlib/transform.h>
#include <dmsdk/dlib/vmath.h>
#include <dmsdk/dlib/intersection.h>
#include <gameobject/gameobject_ddf.h>
#include <graphics/graphics.h>
#include <render/render.h>

#include "../gamesys_private.h"
#include "comp_private.h"

#include <gamesys/gamesys_ddf.h>
#include <gamesys/mesh_ddf.h>
#include <dmsdk/gamesys/render_constants.h>
#include <dmsdk/gamesys/resources/res_material.h>
#include <dmsdk/resource/resource.h>

#include "../resources/res_mesh.h"

DM_PROPERTY_EXTERN(rmtp_Components);
DM_PROPERTY_U32(rmtp_Mesh, 0, PROFILE_PROPERTY_FRAME_RESET, "# components", &rmtp_Components);
DM_PROPERTY_U32(rmtp_MeshVertexCount, 0, PROFILE_PROPERTY_FRAME_RESET, "# vertices", &rmtp_Mesh);
DM_PROPERTY_U32(rmtp_MeshVertexSize, 0, PROFILE_PROPERTY_FRAME_RESET, "size of vertices in bytes", &rmtp_Mesh);

namespace dmGameSystem
{
    using namespace dmVMath;
    using namespace dmGameSystemDDF;

    static const char* MESH_MAX_COUNT_KEY = "mesh.max_count";

    struct MeshComponent
    {
        dmGameObject::HInstance         m_Instance;
        Matrix4                         m_Local;
        Matrix4                         m_World;
        uint32_t                        m_MixedHash;
        HComponentRenderConstants       m_RenderConstants;
        MeshResource*                   m_Resource;
        dmGameSystem::BufferResource*   m_BufferResource;
        TextureResource*                m_Textures[dmRender::RenderObject::MAX_TEXTURE_COUNT];
        MaterialResource*               m_Material;

        // If the buffer resource property has changed (ie not same buffer resource as
        // used by the mesh resource), we need to create a specific vertex declaration
        // and vertbuffer for this buffer resource instead.
        dmGraphics::HVertexDeclaration  m_VertexDeclaration;
        uint32_t                        m_BufferVersion;
        dmhash_t                        m_VertexBufferName; // Actual reference, also across material/resource reloads.

        // Runtime-only property. Each component owns its GPU index buffer and
        // holds a resource reference; the source buffer can be shared from Lua.
        BufferResource*                 m_IndicesResource;
        dmGraphics::HIndexBuffer        m_IndexBuffer;
        dmBuffer::HBuffer               m_IndicesHandle;
        uint32_t                        m_IndicesVersion;
        uint32_t                        m_IndicesVertexCount;
        uint32_t                        m_IndexCount;
        dmGraphics::Type                m_IndexType;
        bool                            m_IndicesValid;
        bool                            m_IndicesDirty;

        /// Component enablement
        uint8_t                         m_Enabled : 1;
        /// Added to update or not
        uint8_t                         m_AddedToUpdate : 1;
        uint8_t                         m_ReHash : 1;
        uint8_t                         :5;
    };

    struct VertexBufferInfo
    {
        dmGraphics::HVertexBuffer m_VertexBuffer;
        uint32_t m_RefCount;
        uint32_t m_Version;
    };

    struct MeshWorld
    {
        dmResource::HFactory               m_ResourceFactory;
        dmObjectPool<MeshComponent*>       m_Components;
        dmArray<dmRender::RenderObject>    m_RenderObjects;
        dmHashTable64<VertexBufferInfo>    m_ResourceToVertexBuffer;
        dmArray<dmGraphics::HVertexBuffer> m_VertexBufferPool;
        dmArray<dmGraphics::HVertexBuffer> m_VertexBufferWorld; // for meshes batched in world space
        dmGraphics::HContext               m_GraphicsContext;
        void*                              m_WorldVertexData;
        size_t                             m_WorldVertexDataSize;

    };

    struct MeshContext
    {
        MeshContext()
        {
            memset(this, 0, sizeof(*this));
        }
        dmRender::HRenderContext    m_RenderContext;
        dmResource::HFactory        m_Factory;
        uint32_t                    m_MaxMeshCount;
    };

    static inline void DeallocVertexBuffer(MeshWorld* world, dmGraphics::HVertexBuffer vertex_buffer)
    {
        if (world->m_VertexBufferPool.Full())
            world->m_VertexBufferPool.OffsetCapacity(4);
        world->m_VertexBufferPool.Push(vertex_buffer);
    }

    static inline dmGraphics::HVertexBuffer AllocVertexBuffer(MeshWorld* world, dmGraphics::HContext graphics_context)
    {
        if (!world->m_VertexBufferPool.Empty())
        {
            dmGraphics::HVertexBuffer vertex_buffer = world->m_VertexBufferPool[world->m_VertexBufferPool.Size()-1];
            world->m_VertexBufferPool.SetSize(world->m_VertexBufferPool.Size()-1);
            return vertex_buffer;
        }

        // TODO: perhaps control stream vs draw better?
        return dmGraphics::NewVertexBuffer(graphics_context, 0, 0x0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
    }

    static VertexBufferInfo* GetVertexBufferInfo(MeshWorld* world, dmhash_t path)
    {
        VertexBufferInfo* info = world->m_ResourceToVertexBuffer.Get(path);
        return info;
    }

    static dmGraphics::HVertexBuffer GetVertexBuffer(MeshWorld* world, dmhash_t path)
    {
        VertexBufferInfo* info = world->m_ResourceToVertexBuffer.Get(path);
        if (!info)
            return 0;
        return info->m_VertexBuffer;
    }

    static void AddVertexBufferInfo(MeshWorld* world, dmhash_t path, dmGraphics::HVertexBuffer vertex_buffer, uint32_t version)
    {
        VertexBufferInfo info;
        info.m_RefCount = 1;
        info.m_VertexBuffer = vertex_buffer;
        info.m_Version = version;
        if (world->m_ResourceToVertexBuffer.Full()) {
            uint32_t capacity = world->m_ResourceToVertexBuffer.Capacity() + 8;
            world->m_ResourceToVertexBuffer.SetCapacity(capacity/3, capacity);
        }
        world->m_ResourceToVertexBuffer.Put(path, info);
    }

    static void DecRefVertexBuffer(MeshWorld* world, dmhash_t path)
    {
        VertexBufferInfo* info = world->m_ResourceToVertexBuffer.Get(path);
        assert(info != 0);

        info->m_RefCount--;
        if (info->m_RefCount == 0) {
            world->m_ResourceToVertexBuffer.Erase(path);
            DeallocVertexBuffer(world, info->m_VertexBuffer);
        }
    }

    static void IncRefVertexBuffer(MeshWorld* world, dmhash_t path)
    {
        VertexBufferInfo* info = world->m_ResourceToVertexBuffer.Get(path);
        assert(info != 0);
        info->m_RefCount++;
    }

    static uint32_t CalcBufferVersion(const MeshComponent* component, BufferResource* br)
    {
        HashState32 state;
        dmHashInit32(&state, false);

        uint32_t version;
        dmBuffer::GetContentVersion(br->m_Buffer, &version);
        dmHashUpdateBuffer32(&state, &br->m_Buffer, sizeof(br->m_Buffer)); // The handle is not a pointer, but a version+index
        dmHashUpdateBuffer32(&state, &version, sizeof(version));

        return dmHashFinal32(&state);
    }

    static void CopyBufferToVertexBuffer(dmBuffer::HBuffer buffer, dmGraphics::HVertexBuffer vertex_buffer, uint32_t vert_size, uint32_t elem_count, dmGraphics::BufferUsage buffer_usage)
    {
        uint8_t* bytes = 0x0;
        uint32_t size = 0;
        dmBuffer::Result r = dmBuffer::GetBytes(buffer, (void**)&bytes, &size);
        assert(r == dmBuffer::RESULT_OK);

        dmGraphics::SetVertexBufferData(vertex_buffer, vert_size * elem_count, bytes, buffer_usage);
    }

    static void CreateVertexBuffer(MeshWorld* world, dmGameSystem::BufferResource* br)
    {
        dmGraphics::HVertexBuffer vertex_buffer = GetVertexBuffer(world, br->m_NameHash);
        if (!vertex_buffer)
        {
            vertex_buffer = AllocVertexBuffer(world, world->m_GraphicsContext);
            AddVertexBufferInfo(world, br->m_NameHash, vertex_buffer, 0); // ref count == 1
        }
        else
        {
            IncRefVertexBuffer(world, br->m_NameHash);
        }
    }

    static void UpdateVertexBuffer(MeshWorld* world, dmGameSystem::BufferResource* br, uint32_t version)
    {
        VertexBufferInfo* info = GetVertexBufferInfo(world, br->m_NameHash);
        if (info->m_Version != version)
        {
            info->m_Version = version;
            CopyBufferToVertexBuffer(br->m_Buffer, info->m_VertexBuffer, br->m_Stride, br->m_ElementCount, dmGraphics::BUFFER_USAGE_STREAM_DRAW);
        }
    }

    static const uint32_t MAX_TEXTURE_COUNT = dmRender::RenderObject::MAX_TEXTURE_COUNT;

    static const dmhash_t PROP_VERTICES = dmHashString64("vertices");
    static const dmhash_t PROP_INDICES = dmHashString64("indices");

    static const uint64_t AABB_HASH = dmHashString64("AABB");

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams* params);

    dmGameObject::CreateResult CompMeshNewWorld(const dmGameObject::ComponentNewWorldParams& params)
    {
        MeshContext* context = (MeshContext*)params.m_Context;

        MeshWorld* world = new MeshWorld();
        uint32_t comp_count = dmMath::Min(params.m_MaxComponentInstances, context->m_MaxMeshCount);
        world->m_ResourceFactory = context->m_Factory;
        world->m_Components.SetCapacity(comp_count);
        world->m_RenderObjects.SetCapacity(comp_count);

        world->m_VertexBufferPool.SetCapacity(0);
        world->m_VertexBufferPool.SetSize(0);

        world->m_WorldVertexData = 0x0;
        world->m_WorldVertexDataSize = 0;

        world->m_GraphicsContext = dmRender::GetGraphicsContext(context->m_RenderContext);

        *params.m_World = world;

        dmResource::RegisterResourceReloadedCallback(context->m_Factory, ResourceReloadedCallback, world);

        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompMeshDeleteWorld(const dmGameObject::ComponentDeleteWorldParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;

        if (world->m_VertexBufferPool.Capacity() < world->m_VertexBufferPool.Size()+world->m_VertexBufferWorld.Size())
            world->m_VertexBufferPool.OffsetCapacity(world->m_VertexBufferWorld.Size());
        world->m_VertexBufferPool.PushArray(world->m_VertexBufferWorld.Begin(), world->m_VertexBufferWorld.Size());
        world->m_VertexBufferWorld.SetSize(0);

        for(uint32_t i = 0; i < world->m_VertexBufferPool.Size(); ++i)
        {
            dmGraphics::DeleteVertexBuffer(world->m_VertexBufferPool[i]);
        }

        if (world->m_WorldVertexData)
        {
            free(world->m_WorldVertexData);
        }

        dmResource::UnregisterResourceReloadedCallback(((MeshContext*)params.m_Context)->m_Factory, ResourceReloadedCallback, world);

        delete world;

        return dmGameObject::CREATE_RESULT_OK;
    }

    static inline dmGameSystem::BufferResource* GetBufferResource(const MeshComponent* component)
    {
        return component->m_BufferResource ? component->m_BufferResource : component->m_Resource->m_BufferResource;
    }

    static inline MaterialResource* GetMaterialResource(const MeshComponent* component, const MeshResource* resource)
    {
        return component->m_Material ? component->m_Material : resource->m_Material;
    }

    static inline dmRender::HMaterial GetMaterial(const MeshComponent* component, const MeshResource* resource)
    {
        return GetMaterialResource(component, resource)->m_Material;
    }

    static void SyncLocalVertexBuffer(MeshWorld* world, MeshComponent* component)
    {
        BufferResource* br = GetBufferResource(component);
        if (component->m_VertexBufferName != br->m_NameHash)
        {
            CreateVertexBuffer(world, br);
            if (component->m_VertexBufferName)
                DecRefVertexBuffer(world, component->m_VertexBufferName);
            component->m_VertexBufferName = br->m_NameHash;
        }
        component->m_BufferVersion = CalcBufferVersion(component, br);
        UpdateVertexBuffer(world, br, component->m_BufferVersion);
    }

    static void ReleaseLocalVertexBuffer(MeshWorld* world, MeshComponent* component)
    {
        if (component->m_VertexBufferName)
        {
            DecRefVertexBuffer(world, component->m_VertexBufferName);
            component->m_VertexBufferName = 0;
        }
    }

    // A single scalar stream is tightly packed by dmBuffer. Stream names are
    // unrestricted. Read the live buffer, since resource.set_buffer can replace
    // its layout, count and handle without replacing the resource itself.
    static bool GetIndexData(MeshWorld* world, BufferResource* br, uint32_t vertex_count,
                            dmGraphics::Type* type, uint32_t* count, void** data, uint32_t* size)
    {
        uint32_t streams = 0, components = 0, stride = 0;
        dmhash_t name = 0;
        dmBuffer::ValueType value_type;
        if (dmBuffer::GetNumStreams(br->m_Buffer, &streams) != dmBuffer::RESULT_OK || streams != 1 ||
            dmBuffer::GetStreamName(br->m_Buffer, 0, &name) != dmBuffer::RESULT_OK ||
            dmBuffer::GetStreamType(br->m_Buffer, name, &value_type, &components) != dmBuffer::RESULT_OK ||
            components != 1 || (value_type != dmBuffer::VALUE_TYPE_UINT16 && value_type != dmBuffer::VALUE_TYPE_UINT32))
        {
            dmLogError("Mesh indices require exactly one scalar uint16 or uint32 stream.");
            return false;
        }
        bool is16 = value_type == dmBuffer::VALUE_TYPE_UINT16;
        if (!dmGraphics::IsIndexBufferFormatSupported(world->m_GraphicsContext,
                is16 ? dmGraphics::INDEXBUFFER_FORMAT_16 : dmGraphics::INDEXBUFFER_FORMAT_32))
        {
            dmLogError("Mesh index format is not supported by the graphics adapter.");
            return false;
        }
        void* stream = 0;
        if (dmBuffer::GetStream(br->m_Buffer, name, &stream, count, &components, &stride) != dmBuffer::RESULT_OK ||
            stride != 1 || dmBuffer::GetBytes(br->m_Buffer, data, size) != dmBuffer::RESULT_OK ||
            (uint64_t)*count * (is16 ? 2 : 4) != *size)
        {
            dmLogError("Mesh indices must be tightly packed.");
            return false;
        }
        for (uint32_t i = 0; i < *count; ++i)
        {
            uint32_t index = is16 ? ((uint16_t*)stream)[i] : ((uint32_t*)stream)[i];
            if (index >= vertex_count)
            {
                dmLogError("Mesh index %u at element %u exceeds vertex count %u (indices are zero-based).", index, i, vertex_count);
                return false;
            }
        }
        *type = is16 ? dmGraphics::TYPE_UNSIGNED_SHORT : dmGraphics::TYPE_UNSIGNED_INT;
        return true;
    }

    static void UpdateIndexBuffer(MeshWorld* world, MeshComponent* component)
    {
        BufferResource* br = component->m_IndicesResource;
        if (!br)
            return;
        uint32_t version = 0;
        dmBuffer::GetContentVersion(br->m_Buffer, &version);
        uint32_t vertex_count = GetBufferResource(component)->m_ElementCount;
        if (!component->m_IndicesDirty && component->m_IndicesHandle == br->m_Buffer &&
            component->m_IndicesVersion == version && component->m_IndicesVertexCount == vertex_count)
            return;

        component->m_IndicesDirty = false;
        component->m_IndicesHandle = br->m_Buffer;
        component->m_IndicesVersion = version;
        component->m_IndicesVertexCount = vertex_count;
        void* data = 0;
        uint32_t size = 0;
        component->m_IndicesValid = GetIndexData(world, br, vertex_count, &component->m_IndexType, &component->m_IndexCount, &data, &size);
        if (!component->m_IndicesValid || !component->m_IndexCount)
            return; // Never draw stale indices or fall back to an expanded draw.
        if (!component->m_IndexBuffer)
            component->m_IndexBuffer = dmGraphics::NewIndexBuffer(world->m_GraphicsContext, size, data, dmGraphics::BUFFER_USAGE_STREAM_DRAW);
        else
            dmGraphics::SetIndexBufferData(component->m_IndexBuffer, size, data, dmGraphics::BUFFER_USAGE_STREAM_DRAW);
        if (!component->m_IndexBuffer)
        {
            dmLogError("Could not create mesh index buffer.");
            component->m_IndicesValid = false;
        }
    }

    static TextureResource* GetTextureResource(const MeshComponent* component, uint32_t texture_unit)
    {
        assert(texture_unit < MAX_TEXTURE_COUNT);
        TextureResource* texture;
        // Overridden textures
        texture = component->m_Textures[texture_unit];
        if (texture)
            return texture;
        // Overridden material
        MaterialResource* material = component->m_Material;
        if (material)
            texture = material->m_Textures[texture_unit];
        if (texture)
            return texture;
        // resource textures
        texture = component->m_Resource->m_Textures[texture_unit];
        if (texture)
            return texture;
        // resource material
        material = component->m_Resource->m_Material;
        if (material)
            texture = material->m_Textures[texture_unit];
        if (texture)
            return texture;
        return 0;
    }

    static inline dmGraphics::HTexture GetTexture(const MeshComponent* component, uint32_t index)
    {
        TextureResource* texture_res = GetTextureResource(component, index);
        return texture_res ? texture_res->m_Texture : 0;
    }

    static inline dmGraphics::HVertexDeclaration GetVertexDeclaration(const MeshComponent* component) {
        return component->m_VertexDeclaration ? component->m_VertexDeclaration : component->m_Resource->m_VertexDeclaration;
    }

    static void ReHash(MeshComponent* component)
    {
        // Hash resource-ptr, material-handle, textures and render constants
        HashState32 state;
        bool reverse = false;
        MeshResource* resource = component->m_Resource;
        dmHashInit32(&state, reverse);
        dmRender::HMaterial material = GetMaterial(component, resource);
        dmHashUpdateBuffer32(&state, &resource->m_PrimitiveType, sizeof(resource->m_PrimitiveType));
        dmHashUpdateBuffer32(&state, &material, sizeof(material));

        // We have to hash individually since we don't know which textures are set as properties
        for (uint32_t i = 0; i < MAX_TEXTURE_COUNT; ++i)
        {
            dmGraphics::HTexture texture = GetTexture(component, i);
            dmHashUpdateBuffer32(&state, &texture, sizeof(texture));
        }

        BufferResource* br = GetBufferResource(component);
        dmHashUpdateBuffer32(&state, &br->m_NameHash, sizeof(br->m_NameHash));

        // Make sure there is a vertex declaration
        // If the mesh uses a buffer that has zero elements we couldn't
        // create a vert declaration, so just skip hashing it here.
        dmGraphics::HVertexDeclaration vert_decl = GetVertexDeclaration(component);
        if (vert_decl) {
            dmGraphics::HashVertexDeclaration(&state, vert_decl);
        }
        if (component->m_RenderConstants)
            dmGameSystem::HashRenderConstants(component->m_RenderConstants, &state);
        component->m_MixedHash = dmHashFinal32(&state);
        component->m_ReHash = 0;
    }

    dmGameObject::CreateResult CompMeshCreate(const dmGameObject::ComponentCreateParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;

        if (world->m_Components.Full())
        {
            ShowFullBufferError("Mesh", MESH_MAX_COUNT_KEY, world->m_Components.Capacity());
            return dmGameObject::CREATE_RESULT_TOO_MANY_COMPONENTS;
        }
        uint32_t index = world->m_Components.Alloc();
        MeshComponent* component = new MeshComponent;
        memset(component, 0, sizeof(MeshComponent));
        world->m_Components.Set(index, component);
        component->m_Instance = params.m_Instance;
        component->m_Local = Matrix4(params.m_Rotation, Vector3(params.m_Position));
        component->m_Resource = (MeshResource*)params.m_Resource;

        component->m_Enabled = 1;
        component->m_World = Matrix4::identity();
        component->m_BufferVersion = 0;

        const Matrix4& go_world = dmGameObject::GetWorldMatrix(component->m_Instance);
        component->m_World = go_world * component->m_Local;

        // Local space uses separate vertex buffers
        if (dmRender::GetMaterialVertexSpace(GetMaterial(component, component->m_Resource)) == dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL)
        {
            SyncLocalVertexBuffer(world, component);
        }

        ReHash(component);

        *params.m_UserData = (uintptr_t)index;
        return dmGameObject::CREATE_RESULT_OK;
    }

    void* CompMeshGetComponent(const dmGameObject::ComponentGetParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;
        return (void*)world->m_Components.Get(params.m_UserData);
    }

    dmGameObject::CreateResult CompMeshDestroy(const dmGameObject::ComponentDestroyParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;

        uint32_t index = (uint32_t)*params.m_UserData;
        MeshComponent* component = world->m_Components.Get(index);
        dmResource::HFactory factory = dmGameObject::GetFactory(params.m_Instance);

        ReleaseLocalVertexBuffer(world, component);

        if (component->m_Material)
        {
            dmResource::Release(factory, component->m_Material);
        }

        for (uint32_t i = 0; i < MAX_TEXTURE_COUNT; ++i) {
            if (component->m_Textures[i]) {
                dmResource::Release(factory, component->m_Textures[i]);
            }
        }

        if (component->m_BufferResource) {
            dmResource::Release(factory, component->m_BufferResource);
        }
        if (component->m_IndicesResource)
            dmResource::Release(factory, component->m_IndicesResource);
        if (component->m_IndexBuffer)
            dmGraphics::DeleteIndexBuffer(component->m_IndexBuffer);
        if (component->m_VertexDeclaration)
            dmGraphics::DeleteVertexDeclaration(component->m_VertexDeclaration);
        if (component->m_RenderConstants)
            dmGameSystem::DestroyRenderConstants(component->m_RenderConstants);

        delete component;
        world->m_Components.Free(index, true);

        return dmGameObject::CREATE_RESULT_OK;
    }

    static void UpdateTransforms(MeshWorld* world)
    {
        DM_PROFILE("UpdateTransforms");

        const dmArray<MeshComponent*>& components = world->m_Components.GetRawObjects();
        uint32_t n = components.Size();
        for (uint32_t i = 0; i < n; ++i)
        {
            MeshComponent* c = components[i];

            // NOTE: texture_set = c->m_Resource might be NULL so it's essential to "continue" here
            if (!c->m_Enabled || !c->m_AddedToUpdate)
                continue;

            const Matrix4& go_world = dmGameObject::GetWorldMatrix(c->m_Instance);
            c->m_World = go_world * c->m_Local;
        }
    }

    dmGameObject::CreateResult CompMeshAddToUpdate(const dmGameObject::ComponentAddToUpdateParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;
        uint32_t index = (uint32_t)*params.m_UserData;
        MeshComponent* component = world->m_Components.Get(index);
        component->m_AddedToUpdate = true;
        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::UpdateResult CompMeshUpdate(const dmGameObject::ComponentsUpdateParams& params, dmGameObject::ComponentsUpdateResult& update_result)
    {
        DM_PROFILE("Update");

        MeshWorld* world = (MeshWorld*)params.m_World;

        const dmArray<MeshComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();

        for (uint32_t i = 0; i < count; ++i)
        {
            MeshComponent& component = *components[i];

            if (!component.m_Enabled || !component.m_AddedToUpdate)
            {
                continue;
            }

            dmRender::HMaterial material = GetMaterial(&component, component.m_Resource);
            if (dmRender::GetMaterialVertexSpace(material) == dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL)
            {
                // Resources and material vertex space can change at runtime or on reload.
                SyncLocalVertexBuffer(world, &component);
                UpdateIndexBuffer(world, &component);
            }
            else
            {
                ReleaseLocalVertexBuffer(world, &component);
            }

            if (component.m_RenderConstants && dmGameSystem::AreRenderConstantsUpdated(component.m_RenderConstants))
                component.m_ReHash = 1;

            if (component.m_ReHash)
            {
                ReHash(&component);
            }
        }

        return dmGameObject::UPDATE_RESULT_OK;
    }

    dmGameObject::UpdateResult CompMeshLateUpdate(const dmGameObject::ComponentsUpdateParams& params, dmGameObject::ComponentsUpdateResult& update_result)
    {
        DM_PROFILE("LateUpdate");
        MeshWorld* world = (MeshWorld*)params.m_World;

        UpdateTransforms(world);
        return dmGameObject::UPDATE_RESULT_OK;
    }

    static void FillRenderObject(dmRender::RenderObject& ro,
        const dmGraphics::PrimitiveType& primitive_type,
        const dmRender::HMaterial& material,
        const TextureResource** textures_resource,
        const TextureResource** textures_component,
        const dmGraphics::HVertexDeclaration& vert_decl,
        const dmGraphics::HVertexBuffer& vert_buffer,
        uint32_t vert_start, uint32_t vert_count,
        const Matrix4& world_transform,
        HComponentRenderConstants constants)
    {
        ro.Init();
        ro.m_VertexDeclaration = vert_decl;
        ro.m_VertexBuffer = vert_buffer;
        ro.m_Material = material;
        ro.m_PrimitiveType = primitive_type;
        ro.m_VertexStart = vert_start;
        ro.m_VertexCount = vert_count;
        ro.m_WorldTransform = world_transform;

        for (uint32_t i = 0; i < dmRender::RenderObject::MAX_TEXTURE_COUNT; ++i)
        {
            if (textures_component[i])
            {
                ro.m_Textures[i] = textures_component[i]->m_Texture;;
            }
            else if (textures_resource[i])
            {
                ro.m_Textures[i] = textures_resource[i]->m_Texture;
            }
        }

        if (constants)
        {
            dmGameSystem::EnableRenderObjectConstants(&ro, constants);
        }
    }

    template<typename T> static void FillAndApply(const Matrix4& matrix, bool is_point, uint8_t component_count, uint32_t count, uint32_t stride, T* raw_data, T* src_stream_data, T* dst_data_ptr)
    {
        // Offset dst_data_ptr if stream isn't first!
        uint32_t ptr_offset = (uint8_t*)src_stream_data - (uint8_t*)raw_data;
        dst_data_ptr = (T*)((uint8_t*)dst_data_ptr + ptr_offset);

        Vector4 v(0.0f);
        float w = is_point ? 1.0f : 0.0f;

        if (component_count == 2) {
            for (int pi = 0; pi < count; ++pi)
            {
                v[0] = (float)src_stream_data[0];
                v[1] = (float)src_stream_data[1];
                v[2] = 0.0f;
                v[3] = w;
                v = matrix * v;
                dst_data_ptr[0] = (T)v[0];
                dst_data_ptr[1] = (T)v[1];
                dst_data_ptr[2] = (T)v[2];

                // Update in/out-ptrs with stride (they will point to next entry in T "list").
                src_stream_data += stride;
                dst_data_ptr += stride;
            }
        } else {
            for (int pi = 0; pi < count; ++pi)
            {
                v[0] = (float)src_stream_data[0];
                v[1] = (float)src_stream_data[1];
                v[2] = (float)src_stream_data[2];
                v[3] = w;
                v = matrix * v;
                dst_data_ptr[0] = (T)v[0];
                dst_data_ptr[1] = (T)v[1];
                dst_data_ptr[2] = (T)v[2];

                // Update in/out-ptrs with stride (they will point to next entry in T "list").
                src_stream_data += stride;
                dst_data_ptr += stride;
            }
        }
    }

    static inline void FillAndApplyStream(const BufferResource* buffer_resource, bool is_point, const Matrix4& matrix, dmhash_t stream_id, dmBufferDDF::ValueType value_type, void* raw_data, void* dst_data_ptr)
    {
        // Get position stream to figure out offset and stride etc
        void* stream_data = 0x0;
        uint32_t count = 0;
        uint32_t components = 0;
        uint32_t stride = 0;
        dmBuffer::Result r = dmBuffer::GetStream(buffer_resource->m_Buffer, stream_id, &stream_data, &count, &components, &stride);
        if (r != dmBuffer::RESULT_OK) {
            dmLogError("Could not get stream %s from buffer when rendering mesh in world space (%d).", dmHashReverseSafe64(stream_id), r);
            return;
        }

        if (!(components == 3 || components == 2)) {
            dmLogError("Rendering mesh components in world space is only supported for streams with 3 or 2 components, %s has %d components.", dmHashReverseSafe64(stream_id), components);
            return;
        }

        switch (value_type)
        {
            case dmBufferDDF::VALUE_TYPE_UINT8:
            FillAndApply<uint8_t>(matrix, is_point, components, count, stride, (uint8_t*)raw_data, (uint8_t*)stream_data, (uint8_t*)dst_data_ptr);
                break;
            case dmBufferDDF::VALUE_TYPE_UINT16:
            FillAndApply<uint16_t>(matrix, is_point, components, count, stride, (uint16_t*)raw_data, (uint16_t*)stream_data, (uint16_t*)dst_data_ptr);
                break;
            case dmBufferDDF::VALUE_TYPE_UINT32:
            FillAndApply<uint32_t>(matrix, is_point, components, count, stride, (uint32_t*)raw_data, (uint32_t*)stream_data, (uint32_t*)dst_data_ptr);
                break;
            case dmBufferDDF::VALUE_TYPE_INT8:
            FillAndApply<int8_t>(matrix, is_point, components, count, stride, (int8_t*)raw_data, (int8_t*)stream_data, (int8_t*)dst_data_ptr);
                break;
            case dmBufferDDF::VALUE_TYPE_INT16:
            FillAndApply<int16_t>(matrix, is_point, components, count, stride, (int16_t*)raw_data, (int16_t*)stream_data, (int16_t*)dst_data_ptr);
                break;
            case dmBufferDDF::VALUE_TYPE_INT32:
            FillAndApply<int32_t>(matrix, is_point, components, count, stride, (int32_t*)raw_data, (int32_t*)stream_data, (int32_t*)dst_data_ptr);
                break;
            case dmBufferDDF::VALUE_TYPE_FLOAT32:
            FillAndApply<float>(matrix, is_point, components, count, stride, (float*)raw_data, (float*)stream_data, (float*)dst_data_ptr);
                break;
            default:
                dmLogError("Stream type (%d) for %s is not supported.", value_type, dmHashReverseSafe64(stream_id));
                break;
        }
    }

    static inline void RenderBatchWorldVS(MeshWorld* world, dmRender::HMaterial material, dmRender::HRenderContext render_context, dmRender::RenderListEntry *buf, uint32_t* begin, uint32_t* end)
    {
        DM_PROFILE("RenderBatchWorld");

        dmGraphics::HVertexBuffer vert_buffer = AllocVertexBuffer(world, world->m_GraphicsContext);
        assert(vert_buffer);

        if (world->m_VertexBufferWorld.Full())
            world->m_VertexBufferWorld.OffsetCapacity(2);
        world->m_VertexBufferWorld.Push(vert_buffer);

        dmRender::RenderObject& ro = *world->m_RenderObjects.End();
        world->m_RenderObjects.SetSize(world->m_RenderObjects.Size()+1);

        const MeshComponent* first = (MeshComponent*) buf[*begin].m_UserData;
        const MeshResource* mr = first->m_Resource;
        const BufferResource* br = GetBufferResource(first);

        // Setup vertex declaration, count and sizes etc.
        // These defaults to values in the mesh and buffer resources,
        // but will be overwritten if the component instance has a "custom"
        // vertices buffer set (ie set via resource properties).
        dmGraphics::HVertexDeclaration vert_decl = mr->m_VertexDeclaration;
        uint32_t vert_size = br->m_Stride;

        // Find out how many elements/vertices all instances in this batch has
        // TODO: We might want to calculate this only when a mesh/buffer changes.
        uint32_t element_count = 0;
        for (uint32_t *i=begin;i!=end;i++)
        {
            const MeshComponent* c = (MeshComponent*) buf[*i].m_UserData;
            const BufferResource* br = GetBufferResource(c);

            element_count += br->m_ElementCount;
        }

        // Allocate a larger scratch buffer if vert count * vert size is larger than current buffer.
        if (world->m_WorldVertexDataSize < vert_size * element_count)
        {
            world->m_WorldVertexDataSize = vert_size * element_count;
            world->m_WorldVertexData = realloc(world->m_WorldVertexData, world->m_WorldVertexDataSize);
        }

        // Fill scratch buffer with data
        void* dst_data_ptr = world->m_WorldVertexData;
        for (uint32_t *i=begin;i!=end;i++)
        {
            const MeshComponent* component = (MeshComponent*) buf[*i].m_UserData;
            const MeshResource* mr = component->m_Resource;
            const BufferResource* br = GetBufferResource(component);

            // No idea of rendering with zero element count.
            if (br->m_ElementCount == 0) {
                continue;
            }

            void* raw_data = 0x0;
            uint32_t size = 0;
            dmBuffer::Result r = dmBuffer::GetBytes(br->m_Buffer, &raw_data, &size);
            if (r != dmBuffer::RESULT_OK) {
                dmLogError("Could not get bytes from buffer when rendering mesh in world space (%d).", r);
                continue;
            }

            // Copy all buffer data
            memcpy(dst_data_ptr, raw_data, size);

            // Modify position stream, if specified
            if (mr->m_PositionStreamId) {
                FillAndApplyStream(br, true, component->m_World, mr->m_PositionStreamId, mr->m_PositionStreamType, raw_data, dst_data_ptr);
            }

            // Modify normal stream, if specified
            if (mr->m_NormalStreamId) {
                Matrix4 normal_matrix = affineInverse(component->m_World);
                normal_matrix = transpose(normal_matrix);
                FillAndApplyStream(br, false, normal_matrix, mr->m_NormalStreamId, mr->m_NormalStreamType, raw_data, dst_data_ptr);
            }

            dst_data_ptr = (void*)((uint8_t*)dst_data_ptr + size);
        }

        DM_PROPERTY_ADD_U32(rmtp_MeshVertexCount, element_count);
        DM_PROPERTY_ADD_U32(rmtp_MeshVertexSize, vert_size * element_count);

        // since they are batched, they have the same settings as the first mesh
        const MeshComponent* component                 = (MeshComponent*) buf[*begin].m_UserData;
        const TextureResource** mesh_resource_textures = (const TextureResource**) mr->m_Textures;
        const TextureResource** component_textures     = (const TextureResource**) component->m_Textures;

        FillRenderObject(ro, mr->m_PrimitiveType, material, mesh_resource_textures, component_textures, vert_decl, vert_buffer, 0, element_count, Matrix4::identity(), first->m_RenderConstants);
        dmGraphics::SetVertexBufferData(vert_buffer, vert_size * element_count, world->m_WorldVertexData, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        dmRender::AddToRender(render_context, &ro);
    }


    static inline void RenderBatchLocalVS(MeshWorld* world, dmRender::HMaterial material, dmRender::HRenderContext render_context, dmRender::RenderListEntry *buf, uint32_t* begin, uint32_t* end)
    {
        DM_PROFILE("RenderBatchLocal");

        for (uint32_t *i=begin;i!=end;i++)
        {
            dmRender::RenderObject& ro = *world->m_RenderObjects.End();
            world->m_RenderObjects.SetSize(world->m_RenderObjects.Size()+1);

            MeshComponent* component = (MeshComponent*) buf[*i].m_UserData;
            SyncLocalVertexBuffer(world, component);
            UpdateIndexBuffer(world, component);
            if (component->m_IndicesResource && (!component->m_IndicesValid || !component->m_IndexCount))
            {
                world->m_RenderObjects.SetSize(world->m_RenderObjects.Size()-1);
                continue;
            }
            const MeshResource* mr = component->m_Resource;
            dmGameSystem::BufferResource* br = GetBufferResource(component);
            VertexBufferInfo* info = world->m_ResourceToVertexBuffer.Get(br->m_NameHash);
            assert(info != 0);

            DM_PROPERTY_ADD_U32(rmtp_MeshVertexCount, br->m_ElementCount);
            DM_PROPERTY_ADD_U32(rmtp_MeshVertexSize, br->m_Stride * br->m_ElementCount);

            dmGraphics::HVertexDeclaration vert_decl = GetVertexDeclaration(component);

            const TextureResource** mesh_resource_textures = (const TextureResource**) mr->m_Textures;
            const TextureResource** component_textures     = (const TextureResource**) component->m_Textures;
            FillRenderObject(ro, mr->m_PrimitiveType, material, mesh_resource_textures, component_textures, vert_decl, info->m_VertexBuffer, 0, br->m_ElementCount, component->m_World, component->m_RenderConstants);
            if (component->m_IndicesResource)
            {
                ro.m_IndexBuffer = component->m_IndexBuffer;
                ro.m_IndexType = component->m_IndexType;
                ro.m_VertexCount = component->m_IndexCount;
                // m_VertexStart is a byte offset for DrawElements. It is zero.
            }
            dmRender::AddToRender(render_context, &ro);
        }
    }

    static void RenderBatch(MeshWorld* world, dmRender::HRenderContext render_context, dmRender::RenderListEntry *buf, uint32_t* begin, uint32_t* end)
    {
        DM_PROFILE("MeshRenderBatch");

        const MeshComponent* first = (MeshComponent*) buf[*begin].m_UserData;
        dmRender::HMaterial material = GetMaterial(first, first->m_Resource);
        switch(dmRender::GetMaterialVertexSpace(material))
        {
            case dmRenderDDF::MaterialDesc::VERTEX_SPACE_WORLD:
                RenderBatchWorldVS(world, material, render_context, buf, begin, end);
            break;

            case dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL:
                RenderBatchLocalVS(world, material, render_context, buf, begin, end);
            break;

            default:
                assert(false);
            break;
        }
    }

    static void RenderListFrustumCulling(dmRender::RenderListVisibilityParams const &params)
    {
        DM_PROFILE("Mesh");

        const dmIntersection::Frustum frustum = *params.m_Frustum;
        uint32_t num_entries = params.m_NumEntries;
        for (uint32_t i = 0; i < num_entries; ++i)
        {
            dmRender::RenderListEntry* entry = &params.m_Entries[i];
            MeshComponent* component_p = (MeshComponent*)entry->m_UserData;

            dmGameSystem::BufferResource* br = GetBufferResource(component_p);

            void* data;
            uint32_t count;
            dmBuffer::ValueType valueType;
            dmBuffer::Result r = dmBuffer::GetMetaData(br->m_Buffer, AABB_HASH, &data, &count, &valueType );

            if (r == dmBuffer::RESULT_METADATA_MISSING)
            {
                // no bounding box available - no culling
                entry->m_Visibility = dmRender::VISIBILITY_FULL;
                continue;
            }
            else if (valueType != dmBuffer::VALUE_TYPE_FLOAT32 || count != 6)
            {
                dmLogError("Invalid AABB metadata. Expected array of 6 floats.");
                entry->m_Visibility = dmRender::VISIBILITY_FULL;
                continue;
            }
            else if (r != dmBuffer::RESULT_OK)
            {
                dmLogError("Error getting AABB for mesh buffer");
                continue;
            }

            dmVMath::Vector3 boundsMin(((float*)data)[0], ((float*)data)[1], ((float*)data)[2] );
            dmVMath::Vector3 boundsMax(((float*)data)[3], ((float*)data)[4], ((float*)data)[5] );

            bool intersect      = dmIntersection::TestFrustumOBB(frustum, component_p->m_World, boundsMin, boundsMax);
            entry->m_Visibility = intersect ? dmRender::VISIBILITY_FULL : dmRender::VISIBILITY_NONE;
        }
    }

    static void RenderListDispatch(dmRender::RenderListDispatchParams const &params)
    {
        MeshWorld *world = (MeshWorld *) params.m_UserData;

        switch (params.m_Operation)
        {
            case dmRender::RENDER_LIST_OPERATION_BEGIN:
            {
                world->m_RenderObjects.SetSize(0);

                if (world->m_VertexBufferPool.Capacity() < world->m_VertexBufferPool.Size()+world->m_VertexBufferWorld.Size())
                    world->m_VertexBufferPool.OffsetCapacity(world->m_VertexBufferWorld.Size());
                world->m_VertexBufferPool.PushArray(world->m_VertexBufferWorld.Begin(), world->m_VertexBufferWorld.Size());
                world->m_VertexBufferWorld.SetSize(0);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_BATCH:
            {
                RenderBatch(world, params.m_Context, params.m_Buf, params.m_Begin, params.m_End);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_END:
            {
                break;
            }
            default:
                assert(false);
                break;
        }
    }

    dmGameObject::UpdateResult CompMeshRender(const dmGameObject::ComponentsRenderParams& params)
    {
        DM_PROFILE("Render");

        MeshContext* context = (MeshContext*)params.m_Context;
        dmRender::HRenderContext render_context = context->m_RenderContext;
        MeshWorld* world = (MeshWorld*)params.m_World;

        const dmArray<MeshComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();

        // Prepare list submit
        dmRender::RenderListEntry* render_list = dmRender::RenderListAlloc(render_context, count);
        dmRender::HRenderListDispatch dispatch = dmRender::RenderListMakeDispatch(render_context, &RenderListDispatch, &RenderListFrustumCulling, world);
        dmRender::RenderListEntry* write_ptr = render_list;

        for (uint32_t i = 0; i < count; ++i)
        {
            MeshComponent& component = *components[i];
            if (!component.m_Enabled || !component.m_AddedToUpdate)
            {
                continue;
            }

            if (component.m_IndicesResource && dmRender::GetMaterialVertexSpace(GetMaterial(&component, component.m_Resource)) != dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL)
            {
                if (component.m_IndicesValid || component.m_IndicesDirty)
                    dmLogError("Mesh indices require a local-space material; world-space indexed meshes are not supported.");
                component.m_IndicesValid = false;
                component.m_IndicesDirty = false;
                continue;
            }

            DM_PROPERTY_ADD_U32(rmtp_Mesh, 1);
            const Vector4 trans = component.m_World.getCol(3);
            write_ptr->m_WorldPosition = Point3(trans.getX(), trans.getY(), trans.getZ());
            write_ptr->m_UserData = (uintptr_t) &component;
            write_ptr->m_BatchKey = component.m_MixedHash;
            write_ptr->m_TagListKey = dmRender::GetMaterialTagListKey(GetMaterial(&component, component.m_Resource));
            write_ptr->m_Dispatch = dispatch;
            write_ptr->m_MinorOrder = 0;
            write_ptr->m_MajorOrder = dmRender::RENDER_ORDER_WORLD;
            ++write_ptr;
        }

        dmRender::RenderListSubmit(render_context, render_list, write_ptr);

        return dmGameObject::UPDATE_RESULT_OK;
    }

    static bool CompMeshGetConstantCallback(void* user_data, dmhash_t name_hash, dmRender::Constant** out_constant)
    {
        MeshComponent* component = (MeshComponent*)user_data;
        return component->m_RenderConstants && dmGameSystem::GetRenderConstant(component->m_RenderConstants, name_hash, out_constant);
    }

    static void CompMeshSetConstantCallback(void* user_data, dmhash_t name_hash, int32_t value_index, uint32_t* element_index, const dmGameObject::PropertyVar& var)
    {
        MeshComponent* component = (MeshComponent*)user_data;
        if (!component->m_RenderConstants)
            component->m_RenderConstants = dmGameSystem::CreateRenderConstants();
        dmGameSystem::SetRenderConstant(component->m_RenderConstants, GetMaterial(component, component->m_Resource), name_hash, value_index, element_index, var);
        component->m_ReHash = 1;
    }

    dmGameObject::UpdateResult CompMeshOnMessage(const dmGameObject::ComponentOnMessageParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;
        MeshComponent* component = world->m_Components.Get(*params.m_UserData);
        if (params.m_Message->m_Id == dmGameObjectDDF::Enable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 1;
        }
        else if (params.m_Message->m_Id == dmGameObjectDDF::Disable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 0;
        }
        else if (params.m_Message->m_Descriptor != 0x0)
        {
            if (params.m_Message->m_Id == dmGameSystemDDF::ResetConstant::m_DDFDescriptor->m_NameHash)
            {
                dmGameSystemDDF::ResetConstant* ddf = (dmGameSystemDDF::ResetConstant*)params.m_Message->m_Data;
                if (component->m_RenderConstants && dmGameSystem::ClearRenderConstant(component->m_RenderConstants, ddf->m_NameHash))
                {
                    component->m_ReHash = 1;
                }
            }
        }
        return dmGameObject::UPDATE_RESULT_OK;
    }

    void CompMeshOnReload(const dmGameObject::ComponentOnReloadParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;
        int index = *params.m_UserData;
        MeshComponent* component = world->m_Components.Get(index);
        component->m_Resource = (MeshResource*)params.m_Resource;
        component->m_ReHash = 1;
        component->m_IndicesDirty = true;
    }

    dmGameObject::PropertyResult CompMeshGetProperty(const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;
        MeshComponent* component = world->m_Components.Get(*params.m_UserData);

        if (params.m_PropertyId == PROP_VERTICES) {
            return GetResourceProperty(dmGameObject::GetFactory(params.m_Instance), GetBufferResource(component), out_value);
        }
        else if (params.m_PropertyId == PROP_INDICES)
        {
            if (!component->m_IndicesResource)
            {
                out_value.m_Variant = dmGameObject::PropertyVar((dmhash_t)0);
                return dmGameObject::PROPERTY_RESULT_OK;
            }
            return GetResourceProperty(dmGameObject::GetFactory(params.m_Instance), component->m_IndicesResource, out_value);
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            return GetResourceProperty(dmGameObject::GetFactory(params.m_Instance), GetMaterialResource(component, component->m_Resource), out_value);
        }

        for (uint32_t i = 0; i < MAX_TEXTURE_COUNT; ++i)
        {
            if (params.m_PropertyId == PROP_TEXTURE[i])
            {
                return GetResourceProperty(dmGameObject::GetFactory(params.m_Instance), GetTextureResource(component, i), out_value);
            }
        }

        int32_t value_index = 0;
        GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

        return GetMaterialConstant(GetMaterial(component, component->m_Resource), params.m_PropertyId, value_index, out_value, true, CompMeshGetConstantCallback, component);
    }

    dmGameObject::PropertyResult CompMeshSetProperty(const dmGameObject::ComponentSetPropertyParams& params)
    {
        MeshWorld* world = (MeshWorld*)params.m_World;
        MeshComponent* component = world->m_Components.Get(*params.m_UserData);

        if (params.m_PropertyId == PROP_INDICES)
        {
            dmResource::HFactory factory = dmGameObject::GetFactory(params.m_Instance);
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_HASH)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
            // hash("") / hash value zero detaches the buffer.
            if (params.m_Value.m_Hash == 0)
            {
                if (component->m_IndicesResource)
                    dmResource::Release(factory, component->m_IndicesResource);
                if (component->m_IndexBuffer)
                    dmGraphics::DeleteIndexBuffer(component->m_IndexBuffer);
                component->m_IndicesResource = 0;
                component->m_IndexBuffer = 0;
                component->m_IndicesValid = false;
                component->m_IndicesDirty = true;
                return dmGameObject::PROPERTY_RESULT_OK;
            }
            if (dmRender::GetMaterialVertexSpace(GetMaterial(component, component->m_Resource)) != dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL)
            {
                dmLogError("Mesh indices require a local-space material; world-space indexed meshes are not supported.");
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }
            // Validate before replacing the current property or releasing its reference.
            BufferResource* candidate = 0;
            dmGameObject::PropertyResult result = SetResourceProperty(factory, params.m_Value, BUFFER_EXT_HASH, (void**)&candidate);
            if (result != dmGameObject::PROPERTY_RESULT_OK)
                return result;
            dmGraphics::Type type;
            uint32_t count, size;
            void* data;
            bool valid = GetIndexData(world, candidate, GetBufferResource(component)->m_ElementCount, &type, &count, &data, &size);
            if (!valid)
            {
                dmResource::Release(factory, candidate);
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }
            if (component->m_IndicesResource)
                dmResource::Release(factory, component->m_IndicesResource);
            component->m_IndicesResource = candidate;
            component->m_IndicesDirty = true;
            UpdateIndexBuffer(world, component);
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_VERTICES)
        {
            BufferResource* prev_buffer_resource = GetBufferResource(component);
            BufferResource* prev_custom_buffer_resource = component->m_BufferResource;

            dmGameObject::PropertyResult res = SetResourceProperty(dmGameObject::GetFactory(params.m_Instance), params.m_Value, BUFFER_EXT_HASH, (void**)&component->m_BufferResource);
            component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;

            if (res == dmGameObject::PROPERTY_RESULT_OK)
            {
                BufferResource* br = GetBufferResource(component);
                component->m_BufferVersion = CalcBufferVersion(component, br);

                // If the buffer resource was changed, we might need to recreate the vertex declaration.
                if (!prev_custom_buffer_resource || (component->m_BufferResource != prev_buffer_resource)) {

                    // Perhaps figure our a way to avoid recreating the same vertex declarations all the time? (If if it's worth it?)
                    dmGraphics::HVertexDeclaration new_vert_decl;
                    bool r = dmGameSystem::BuildVertexDeclaration(component->m_BufferResource, &new_vert_decl);
                    if (!r) {
                        dmLogError("Error while building vertex declaration from new resource.");
                        return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
                    }

                    // Delete old vertex declaration
                    if (component->m_VertexDeclaration) {
                        dmGraphics::DeleteVertexDeclaration(component->m_VertexDeclaration);
                    }

                    component->m_VertexDeclaration = new_vert_decl;
                }

                if (dmRender::GetMaterialVertexSpace(GetMaterial(component, component->m_Resource)) == dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL)
                {
                    SyncLocalVertexBuffer(world, component);
                }
            }

            return res;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            if (component->m_IndicesResource)
            {
                MaterialResource* candidate = 0;
                dmResource::HFactory factory = dmGameObject::GetFactory(params.m_Instance);
                dmGameObject::PropertyResult result = SetResourceProperty(factory, params.m_Value, MATERIAL_EXT_HASH, (void**)&candidate);
                if (result != dmGameObject::PROPERTY_RESULT_OK)
                    return result;
                bool local = dmRender::GetMaterialVertexSpace(candidate->m_Material) == dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL;
                dmResource::Release(factory, candidate);
                if (!local)
                {
                    dmLogError("Cannot set a world-space material while mesh indices are attached. Clear indices first.");
                    return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
                }
            }
            dmGameObject::PropertyResult res = SetResourceProperty(dmGameObject::GetFactory(params.m_Instance), params.m_Value, MATERIAL_EXT_HASH, (void**)&component->m_Material);
            component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;

            bool new_material_local = dmRender::GetMaterialVertexSpace(GetMaterial(component, component->m_Resource)) == dmRenderDDF::MaterialDesc::VERTEX_SPACE_LOCAL;

            if (res == dmGameObject::PROPERTY_RESULT_OK)
            {
                if (new_material_local)
                    SyncLocalVertexBuffer(world, component);
                else
                    ReleaseLocalVertexBuffer(world, component);
                component->m_IndicesDirty = true;
            }
            return res;
        }

        for(uint32_t i = 0; i < MAX_TEXTURE_COUNT; ++i)
        {
            if(params.m_PropertyId == PROP_TEXTURE[i])
            {
                dmGameObject::PropertyResult res = SetResourceProperty(dmGameObject::GetFactory(params.m_Instance), params.m_Value, TEXTURE_EXT_HASH, (void**)&component->m_Textures[i]);
                component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;
                return res;
            }
        }

        int32_t value_index = 0;
        GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

        dmGameObject::PropertyResult res = SetMaterialConstant(GetMaterial(component, component->m_Resource), params.m_PropertyId, params.m_Value, value_index, CompMeshSetConstantCallback, component);
        component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;

        return res;
    }

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams* params)
    {
        MeshWorld* world = (MeshWorld*) params->m_UserData;
        const dmArray<MeshComponent*>& components = world->m_Components.GetRawObjects();
        uint32_t n = components.Size();

        const void* resource = dmResource::GetResource(params->m_Resource);
        for (uint32_t i = 0; i < n; ++i)
        {
            MeshComponent* component = components[i];
            if (component->m_Resource)
            {
                const MaterialResource* material = GetMaterialResource(component, component->m_Resource);
                const dmGameSystem::BufferResource* buffer_resource = GetBufferResource(component);
                if (component->m_Resource == resource ||
                   material == resource ||
                   buffer_resource == resource ||
                   component->m_IndicesResource == resource)
                {
                    component->m_ReHash = 1;
                    component->m_IndicesDirty = true;
                    continue;
                }

                for (uint32_t i = 0; i < MAX_TEXTURE_COUNT; ++i)
                {
                    dmGraphics::HTexture texture = GetTexture(component, i);
                    if (texture == (uintptr_t) resource)
                    {
                        component->m_ReHash = 1;
                        break;
                    }
                }
            }
        }
    }

    static bool CompMeshIterPropertiesGetNext(dmGameObject::SceneNodePropertyIterator* pit)
    {
        MeshWorld* world = (MeshWorld*)pit->m_Node->m_ComponentWorld;
        MeshComponent* component = world->m_Components.Get(pit->m_Node->m_Component);

        uint64_t index = pit->m_Next++;

        uint32_t num_bool_properties = 1;
        if (index < num_bool_properties)
        {
            if (index == 0)
            {
                pit->m_Property.m_Type = dmGameObject::SCENE_NODE_PROPERTY_TYPE_BOOLEAN;
                pit->m_Property.m_Value.m_Bool = component->m_Enabled;
                pit->m_Property.m_NameHash = dmHashString64("enabled");
            }
            return true;
        }
        index -= num_bool_properties;

        return false;
    }

    void CompMeshIterProperties(dmGameObject::SceneNodePropertyIterator* pit, dmGameObject::SceneNode* node)
    {
        assert(node->m_Type == dmGameObject::SCENE_NODE_TYPE_COMPONENT);
        assert(node->m_ComponentType != 0);
        pit->m_Node = node;
        pit->m_Next = 0;
        pit->m_FnIterateNext = CompMeshIterPropertiesGetNext;
    }

    static dmGameObject::Result CompMeshTypeCreate(const dmGameObject::ComponentTypeCreateCtx* ctx, dmGameObject::ComponentType* type)
    {
        MeshContext* mesh_context = new MeshContext;
        HContextRegistry context_registry = dmGameObject::ComponentGetContextRegistry(ctx);
        mesh_context->m_Factory = ctx->m_Factory;
        mesh_context->m_RenderContext = (dmRender::HRenderContext) ContextRegistryGet(context_registry, RENDER_CONTEXT_NAME);
        mesh_context->m_MaxMeshCount = dmConfigFile::GetInt(ctx->m_Config, MESH_MAX_COUNT_KEY, 128);

        ComponentTypeSetPrio(type, 725);

        ComponentTypeSetContext(type, mesh_context);
        ComponentTypeSetNewWorldFn(type, CompMeshNewWorld);
        ComponentTypeSetDeleteWorldFn(type, CompMeshDeleteWorld);
        ComponentTypeSetCreateFn(type, CompMeshCreate);
        ComponentTypeSetDestroyFn(type, CompMeshDestroy);
        ComponentTypeSetAddToUpdateFn(type, CompMeshAddToUpdate);
        ComponentTypeSetUpdateFn(type, CompMeshUpdate);
        ComponentTypeSetLateUpdateFn(type, CompMeshLateUpdate);
        ComponentTypeSetRenderFn(type, CompMeshRender);
        ComponentTypeSetOnMessageFn(type, CompMeshOnMessage);
        ComponentTypeSetGetPropertyFn(type, CompMeshGetProperty);
        ComponentTypeSetSetPropertyFn(type, CompMeshSetProperty);
        ComponentTypeSetGetFn(type, CompMeshGetComponent);

        ComponentTypeSetPropertyIteratorFn(type, CompMeshIterProperties);

        return dmGameObject::RESULT_OK;
    }

    static dmGameObject::Result CompMeshTypeDestroy(const dmGameObject::ComponentTypeCreateCtx* ctx, dmGameObject::ComponentType* type)
    {
        MeshContext* mesh_context = (MeshContext*)dmGameObject::ComponentTypeGetContext(type);
        if (!mesh_context)
        {
            // if the initialization process failed (e.g. unit tests)
            return dmGameObject::RESULT_OK;
        }

        delete mesh_context;

        return dmGameObject::RESULT_OK;
    }

}

DM_DECLARE_COMPONENT_TYPE(ComponentTypeMesh, "meshc", dmGameSystem::CompMeshTypeCreate, dmGameSystem::CompMeshTypeDestroy);
