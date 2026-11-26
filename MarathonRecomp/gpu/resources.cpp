#include "resources.h"
#include "render_command.h"
#include "surface_variant.h"
#include "video_internal.h"
#include "guest_convert.h"

#include <algorithm>
#include <bit>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <user/config.h>
#include <utils/host_thread.h>

#ifdef MARATHON_RECOMP_IOS
#include <os/ios/memory_ios.h>
#endif

std::atomic<uint32_t> g_bufferUploadCount = 0;

uint32_t ComputeTexturePitch(GuestTexture* texture)
{
    const uint32_t safeW = std::max(1u, texture->width);
    return (safeW * RenderFormatSize(texture->format) + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
}

GuestTexture* CreateTexture(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool, uint32_t type)
{
    ResourceType resourceType;

    switch (type)
    {
    case 17:
        resourceType = ResourceType::VolumeTexture;
        break;
    case 19:
        resourceType = ResourceType::ArrayTexture;
        break;
    default:
        resourceType = ResourceType::Texture;
        break;
    }

    const uint32_t safeWidth = std::max(1u, width);
    const uint32_t safeHeight = std::max(1u, height);
    const uint32_t safeDepth = std::max(1u, depth);
    const uint32_t maxValidMips = static_cast<uint32_t>(std::bit_width(std::max({ 1u, safeWidth, safeHeight, resourceType == ResourceType::VolumeTexture ? safeDepth : 1u })));
    const uint32_t safeLevels = std::clamp(levels == 0 ? 1u : levels, 1u, maxValidMips);

    RenderFormat hostFormat = ConvertFormat(format);
#ifdef MARATHON_RECOMP_IOS
    if ((hostFormat == RenderFormat::BC1_UNORM || hostFormat == RenderFormat::BC3_UNORM) && !os::ios::SupportsNativeBCTextures())
    {
        hostFormat = RenderFormat::R8G8B8A8_UNORM;
    }
#endif

    RenderTextureDesc desc;
    desc.dimension = resourceType == ResourceType::VolumeTexture ? RenderTextureDimension::TEXTURE_3D : RenderTextureDimension::TEXTURE_2D;
    desc.width = safeWidth;
    desc.height = safeHeight;
    desc.mipLevels = safeLevels;
    desc.format = hostFormat;

    if (resourceType == ResourceType::ArrayTexture) {
        desc.arraySize = safeDepth;
        desc.depth = 1;
    } else {
        desc.depth = safeDepth;
        desc.arraySize = 1;
    }

    if (RenderFormatIsDepth(desc.format))
        desc.flags = RenderTextureFlag::DEPTH_TARGET;
    else if (usage != 0)
        desc.flags = RenderTextureFlag::RENDER_TARGET;
    else
        desc.flags = RenderTextureFlag::NONE;

    RenderTextureViewDesc viewDesc;
    viewDesc.format = desc.format;
    viewDesc.dimension = resourceType == ResourceType::VolumeTexture ? RenderTextureViewDimension::TEXTURE_3D : RenderTextureViewDimension::TEXTURE_2D;
    viewDesc.mipLevels = safeLevels;

    switch (format)
    {
    case D3DFMT_D24FS8:
    case D3DFMT_D24S8:
    case D3DFMT_L8:
    case D3DFMT_L8_2:
        viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R, RenderSwizzle::R, RenderSwizzle::ONE);
        break;

    case D3DFMT_X8R8G8B8:
        viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::G, RenderSwizzle::B, RenderSwizzle::A, RenderSwizzle::ONE);
        break;
    }

    const auto texture = g_userHeap.AllocPhysical<GuestTexture>();

    texture->type = resourceType;
    texture->textureHolder = Video::GetRenderDevice()->createTexture(desc);
    texture->texture = texture->textureHolder.get();
    texture->textureView = texture->texture->createTextureView(viewDesc);

    texture->width = safeWidth;
    texture->height = safeHeight;
    texture->depth = safeDepth;
    texture->format = desc.format;
    texture->guestFormat = format;
    texture->mipLevels = viewDesc.mipLevels;
    texture->viewDimension = viewDesc.dimension;
    texture->allocatedBytes = static_cast<uint64_t>(ComputeTexturePitch(texture)) * safeHeight * safeDepth;
    g_liveTextureBytes.fetch_add(texture->allocatedBytes, std::memory_order_relaxed);
    texture->descriptorIndex = g_textureDescriptorAllocator.allocate();

    g_textureDescriptorSet->setTexture(texture->descriptorIndex, texture->texture, RenderTextureLayout::SHADER_READ, texture->textureView.get());

#ifdef _DEBUG
    texture->texture->setName(fmt::format("Texture {:X}", g_memory.MapVirtual(texture)));
#endif

    if (desc.flags != RenderTextureFlag::NONE)
    {
        DiscardTexture(texture, desc.flags == RenderTextureFlag::RENDER_TARGET ?
            RenderTextureLayout::COLOR_WRITE : RenderTextureLayout::DEPTH_WRITE);
    }

    return texture;
}

GuestSurface* CreateSurface(uint32_t width, uint32_t height, uint32_t format, uint32_t multiSample, const GuestSurfaceCreateParams* params)
{
    const uint32_t safeWidth = std::max(1u, width);
    const uint32_t safeHeight = std::max(1u, height);
    const RenderFormat renderFormat = ConvertFormat(format);
    const RenderSampleCounts sampleCount = multiSample != 0 ? GetRenderSampleCount() : RenderSampleCount::COUNT_1;
    const bool isDepthStencil = RenderFormatIsDepth(renderFormat);

    const auto surface = g_userHeap.AllocPhysical<GuestSurface>();

    surface->type = isDepthStencil ? ResourceType::DepthStencil : ResourceType::RenderTarget;
    surface->width = safeWidth;
    surface->height = safeHeight;
    surface->format = renderFormat;
    surface->guestFormat = format;
    surface->multiSampleType = multiSample;
    surface->sampleCount = sampleCount;

    bool created = false;

    if (params != nullptr)
    {
        // The view shares the variant's host resources without owning them.
        surface->variant = AcquireSurfaceVariant(params->base, safeWidth, safeHeight, format, sampleCount, created);
        surface->texture = surface->variant->textureHolder.get();
        surface->descriptorIndex = surface->variant->descriptorIndex;
    }
    else
    {
        // No EDRAM placement information: the surface owns its host texture.
        const auto desc = MakeSurfaceTextureDesc(safeWidth, safeHeight, renderFormat, sampleCount);
        surface->textureHolder = Video::GetRenderDevice()->createTexture(desc);
        surface->texture = surface->textureHolder.get();

        RenderTextureViewDesc viewDesc;
        viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
        viewDesc.format = renderFormat;
        viewDesc.mipLevels = 1;
        surface->textureView = surface->textureHolder->createTextureView(viewDesc);
        surface->descriptorIndex = g_textureDescriptorAllocator.allocate();
        g_textureDescriptorSet->setTexture(surface->descriptorIndex, surface->textureHolder.get(), RenderTextureLayout::SHADER_READ, surface->textureView.get());

    #ifdef _DEBUG
        surface->texture->setName(fmt::format("{} {:X}", isDepthStencil ? "Depth Stencil" : "Render Target", g_memory.MapVirtual(surface)));
    #endif

        created = true;
    }

    if (created)
    {
        DiscardTexture(surface, isDepthStencil ?
            RenderTextureLayout::DEPTH_WRITE : RenderTextureLayout::COLOR_WRITE);
    }

    return surface;
}

void DestructResource(GuestResource* resource)
{
    g_renderQueue.enqueue(RenderCommands::DestructResource { resource });
}

void ReleaseResource(GuestResource* resource)
{
    resource->Release();

    if (resource->refCount == 0)
        DestructResource(resource);
}

static void LockTextureRect(GuestTexture* texture, uint32_t, GuestLockedRect* lockedRect)
{
    uint32_t pitch = 0;
    uint32_t slicePitch = 0;

    if ((texture->guestFormat == D3DFMT_DXT1 || texture->guestFormat == D3DFMT_DXT4) &&
        texture->format == RenderFormat::R8G8B8A8_UNORM)
    {
        const uint32_t blocksX = (std::max(1u, texture->width) + 3u) / 4u;
        const uint32_t blocksY = (std::max(1u, texture->height) + 3u) / 4u;
        const uint32_t blockBytes = (texture->guestFormat == D3DFMT_DXT1) ? 8u : 16u;
        pitch = (blocksX * blockBytes + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
        slicePitch = pitch * blocksY;
    }
    else
    {
        pitch = ComputeTexturePitch(texture);
        slicePitch = pitch * std::max(1u, texture->height);
    }

    if (texture->mappedMemory == nullptr)
        texture->mappedMemory = g_userHeap.AllocPhysical(slicePitch, 0x10);

    lockedRect->pitch = pitch;
    lockedRect->bits = g_memory.MapVirtual(texture->mappedMemory);
}

static void UnlockTextureRect(GuestTexture* texture)
{
    assert(HostThread::GetCurrentID() == g_mainThreadId);

    g_renderQueue.enqueue(RenderCommands::UnlockTextureRect { texture });
}

static RenderHeapType GetBufferHeapType()
{
    return Video::GetRenderDevice()->getCapabilities().gpuUploadHeap ? RenderHeapType::GPU_UPLOAD : RenderHeapType::DEFAULT;
}

static GuestBuffer* CreateVertexBuffer(uint32_t length)
{
    auto buffer = g_userHeap.AllocPhysical<GuestBuffer>();
    buffer->type = ResourceType::VertexBuffer;
    buffer->buffer = Video::GetRenderDevice()->createBuffer(RenderBufferDesc::VertexBuffer(length, GetBufferHeapType(), RenderBufferFlag::INDEX));
    buffer->dataSize = length;
#ifdef _DEBUG
    buffer->buffer->setName(fmt::format("Vertex Buffer {:X}", g_memory.MapVirtual(buffer)));
#endif
    return buffer;
}

static GuestBuffer* CreateIndexBuffer(uint32_t length, uint32_t, uint32_t format)
{
    auto buffer = g_userHeap.AllocPhysical<GuestBuffer>();
    buffer->type = ResourceType::IndexBuffer;
    buffer->buffer = Video::GetRenderDevice()->createBuffer(RenderBufferDesc::IndexBuffer(length, GetBufferHeapType()));
    buffer->dataSize = length;
    buffer->format = ConvertFormat(format);
    buffer->guestFormat = format;
#ifdef _DEBUG
    buffer->buffer->setName(fmt::format("Index Buffer {:X}", g_memory.MapVirtual(buffer)));
#endif
    return buffer;
}

template<typename T>
static void UnlockBuffer(GuestBuffer* buffer)
{
    if (buffer->lockedReadOnly)
        return;

    auto copyBuffer = [&](T* dest)
    {
        auto src = static_cast<const T*>(buffer->mappedMemory);

        for (size_t i = 0; i < buffer->dataSize; i += sizeof(T))
        {
            *dest = ByteSwap(*src);
            ++dest;
            ++src;
        }
    };

    if (Video::GetRenderDevice()->getCapabilities().gpuUploadHeap)
    {
        copyBuffer(static_cast<T*>(buffer->buffer->map()));
        buffer->buffer->unmap();
    }
    else
    {
        auto uploadBuffer = Video::GetRenderDevice()->createBuffer(RenderBufferDesc::UploadBuffer(buffer->dataSize));
        copyBuffer(static_cast<T*>(uploadBuffer->map()));
        uploadBuffer->unmap();

        Video::ExecuteCopyCommandList([&](RenderCommandList* in_pCommandList)
        {
            in_pCommandList->copyBufferRegion(buffer->buffer->at(0), uploadBuffer->at(0), buffer->dataSize);
        });
    }

    ++g_bufferUploadCount;
}

static void UnlockVertexBuffer(GuestBuffer* buffer)
{
    UnlockBuffer<uint32_t>(buffer);
}

static void UnlockIndexBuffer(GuestBuffer* buffer)
{
    if (buffer->guestFormat == D3DFMT_INDEX32)
        UnlockBuffer<uint32_t>(buffer);
    else
        UnlockBuffer<uint16_t>(buffer);
}

static void* LockBuffer(GuestBuffer* buffer, uint32_t flags)
{
    buffer->lockedReadOnly = (flags & 0x10) != 0;

    if (buffer->mappedMemory == nullptr)
        buffer->mappedMemory = g_userHeap.AllocPhysical(buffer->dataSize, 0x10);

    return buffer->mappedMemory;
}

static void* LockVertexBuffer(GuestBuffer* buffer, uint32_t, uint32_t, uint32_t flags)
{
    return LockBuffer(buffer, flags);
}

static void* LockIndexBuffer(GuestBuffer* buffer, uint32_t, uint32_t, uint32_t flags)
{
    return LockBuffer(buffer, flags);
}

static void GetSurfaceDesc(GuestSurface* surface, GuestSurfaceDesc* desc)
{
    if (surface->width == 0 && surface->height == 0) {
        LOGF_WARNING("Encountered surface with size 0: {:p} {:d} {:d} \n", reinterpret_cast<void*>(desc), surface->width, surface->height);
        __builtin_trap();
    }
    desc->width = surface->width;
    desc->height = surface->height;
    desc->format = surface->guestFormat;
    desc->type = 4; // D3DRTYPE_SURFACE
    desc->multiSampleType = surface->multiSampleType;
    desc->multiSampleQuality = 0;
    desc->usage = 0;
}

static void GetVertexDeclaration(const GuestVertexDeclaration* vertexDeclaration, GuestVertexElement* vertexElements, be<uint32_t>* count)
{
    memcpy(vertexElements, vertexDeclaration->vertexElements.get(), vertexDeclaration->vertexElementCount * sizeof(GuestVertexElement));
    *count = vertexDeclaration->vertexElementCount;
}

// Needed for correct clearing of index buffer
static bool IsSet() {
    return true;
}

int GetType(GuestResource* resource)
{
    if (resource->type == ResourceType::Texture) return 3;
    if (resource->type == ResourceType::VolumeTexture) return 17;
    if (resource->type == ResourceType::ArrayTexture) return 19;

    LOGF_WARNING("Unmapped guest resource type {:d}!", (int32_t)resource->type);
    __builtin_trap();
    return 0;
}

GUEST_FUNCTION_HOOK(sub_8253AE98, DestructResource);

GUEST_FUNCTION_HOOK(sub_8253A740, LockTextureRect);
GUEST_FUNCTION_HOOK(sub_82538D30, UnlockTextureRect);

GUEST_FUNCTION_HOOK(sub_8253A8D8, CreateTexture);
GUEST_FUNCTION_HOOK(sub_8253A9F8, CreateSurface);

GUEST_FUNCTION_HOOK(sub_8253B5D0, LockVertexBuffer);
GUEST_FUNCTION_HOOK(sub_8253B6F0, LockIndexBuffer);

GUEST_FUNCTION_HOOK(sub_8253B630, UnlockVertexBuffer);
GUEST_FUNCTION_HOOK(sub_8253B750, UnlockIndexBuffer);

GUEST_FUNCTION_HOOK(sub_8253B508, CreateVertexBuffer);
GUEST_FUNCTION_HOOK(sub_8253B640, CreateIndexBuffer);

GUEST_FUNCTION_HOOK(sub_8253AB20, GetSurfaceDesc);
GUEST_FUNCTION_HOOK(sub_825471F8, GetVertexDeclaration);

GUEST_FUNCTION_HOOK(sub_8253B760, IsSet);

GUEST_FUNCTION_HOOK(sub_8253AE08, GetType);
