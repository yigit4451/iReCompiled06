#include "video.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <algorithm>
#include <bit>
#include <bc_diff.h>
#include <decompressor.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <res/bc_diff/button_bc_diff.bin.h>
#include <Marathon.h>

static RenderTextureDimension ConvertTextureDimension(ddspp::TextureType type)
{
    switch (type)
    {
    case ddspp::Texture1D:
        return RenderTextureDimension::TEXTURE_1D;
    case ddspp::Texture2D:
    case ddspp::Cubemap:
        return RenderTextureDimension::TEXTURE_2D;
    case ddspp::Texture3D:
        return RenderTextureDimension::TEXTURE_3D;
    default:
        assert(false && "Unknown texture type from DDS.");
        return RenderTextureDimension::UNKNOWN;
    }
}

static RenderTextureViewDimension ConvertTextureViewDimension(ddspp::TextureType type)
{
    switch (type)
    {
    case ddspp::Texture1D:
        return RenderTextureViewDimension::TEXTURE_1D;
    case ddspp::Texture2D:
        return RenderTextureViewDimension::TEXTURE_2D;
    case ddspp::Texture3D:
        return RenderTextureViewDimension::TEXTURE_3D;
    case ddspp::Cubemap:
        return RenderTextureViewDimension::TEXTURE_CUBE;
    default:
        assert(false && "Unknown texture type from DDS.");
        return RenderTextureViewDimension::UNKNOWN;
    }
}

static RenderFormat ConvertDXGIFormat(ddspp::DXGIFormat format)
{
    switch (format)
    {
    case ddspp::R32G32B32A32_TYPELESS:
        return RenderFormat::R32G32B32A32_TYPELESS;
    case ddspp::R32G32B32A32_FLOAT:
        return RenderFormat::R32G32B32A32_FLOAT;
    case ddspp::R32G32B32A32_UINT:
        return RenderFormat::R32G32B32A32_UINT;
    case ddspp::R32G32B32A32_SINT:
        return RenderFormat::R32G32B32A32_SINT;
    case ddspp::R32G32B32_TYPELESS:
        return RenderFormat::R32G32B32_TYPELESS;
    case ddspp::R32G32B32_FLOAT:
        return RenderFormat::R32G32B32_FLOAT;
    case ddspp::R32G32B32_UINT:
        return RenderFormat::R32G32B32_UINT;
    case ddspp::R32G32B32_SINT:
        return RenderFormat::R32G32B32_SINT;
    case ddspp::R16G16B16A16_TYPELESS:
        return RenderFormat::R16G16B16A16_TYPELESS;
    case ddspp::R16G16B16A16_FLOAT:
        return RenderFormat::R16G16B16A16_FLOAT;
    case ddspp::R16G16B16A16_UNORM:
        return RenderFormat::R16G16B16A16_UNORM;
    case ddspp::R16G16B16A16_UINT:
        return RenderFormat::R16G16B16A16_UINT;
    case ddspp::R16G16B16A16_SNORM:
        return RenderFormat::R16G16B16A16_SNORM;
    case ddspp::R16G16B16A16_SINT:
        return RenderFormat::R16G16B16A16_SINT;
    case ddspp::R32G32_TYPELESS:
        return RenderFormat::R32G32_TYPELESS;
    case ddspp::R32G32_FLOAT:
        return RenderFormat::R32G32_FLOAT;
    case ddspp::R32G32_UINT:
        return RenderFormat::R32G32_UINT;
    case ddspp::R32G32_SINT:
        return RenderFormat::R32G32_SINT;
    case ddspp::R8G8B8A8_TYPELESS:
        return RenderFormat::R8G8B8A8_TYPELESS;
    case ddspp::R8G8B8A8_UNORM:
        return RenderFormat::R8G8B8A8_UNORM;
    case ddspp::R8G8B8A8_UINT:
        return RenderFormat::R8G8B8A8_UINT;
    case ddspp::R8G8B8A8_SNORM:
        return RenderFormat::R8G8B8A8_SNORM;
    case ddspp::R8G8B8A8_SINT:
        return RenderFormat::R8G8B8A8_SINT;
    case ddspp::B8G8R8A8_UNORM:
        return RenderFormat::B8G8R8A8_UNORM;
    case ddspp::B8G8R8X8_UNORM:
        return RenderFormat::B8G8R8A8_UNORM;
    case ddspp::R16G16_TYPELESS:
        return RenderFormat::R16G16_TYPELESS;
    case ddspp::R16G16_FLOAT:
        return RenderFormat::R16G16_FLOAT;
    case ddspp::R16G16_UNORM:
        return RenderFormat::R16G16_UNORM;
    case ddspp::R16G16_UINT:
        return RenderFormat::R16G16_UINT;
    case ddspp::R16G16_SNORM:
        return RenderFormat::R16G16_SNORM;
    case ddspp::R16G16_SINT:
        return RenderFormat::R16G16_SINT;
    case ddspp::R32_TYPELESS:
        return RenderFormat::R32_TYPELESS;
    case ddspp::D32_FLOAT:
        return RenderFormat::D32_FLOAT;
    case ddspp::R32_FLOAT:
        return RenderFormat::R32_FLOAT;
    case ddspp::R32_UINT:
        return RenderFormat::R32_UINT;
    case ddspp::R32_SINT:
        return RenderFormat::R32_SINT;
    case ddspp::R8G8_TYPELESS:
        return RenderFormat::R8G8_TYPELESS;
    case ddspp::R8G8_UNORM:
        return RenderFormat::R8G8_UNORM;
    case ddspp::R8G8_UINT:
        return RenderFormat::R8G8_UINT;
    case ddspp::R8G8_SNORM:
        return RenderFormat::R8G8_SNORM;
    case ddspp::R8G8_SINT:
        return RenderFormat::R8G8_SINT;
    case ddspp::R16_TYPELESS:
        return RenderFormat::R16_TYPELESS;
    case ddspp::R16_FLOAT:
        return RenderFormat::R16_FLOAT;
    case ddspp::D16_UNORM:
        return RenderFormat::D16_UNORM;
    case ddspp::R16_UNORM:
        return RenderFormat::R16_UNORM;
    case ddspp::R16_UINT:
        return RenderFormat::R16_UINT;
    case ddspp::R16_SNORM:
        return RenderFormat::R16_SNORM;
    case ddspp::R16_SINT:
        return RenderFormat::R16_SINT;
    case ddspp::R8_TYPELESS:
        return RenderFormat::R8_TYPELESS;
    case ddspp::R8_UNORM:
    case ddspp::A8_UNORM:
        return RenderFormat::R8_UNORM;
    case ddspp::R8_UINT:
        return RenderFormat::R8_UINT;
    case ddspp::R8_SNORM:
        return RenderFormat::R8_SNORM;
    case ddspp::R8_SINT:
        return RenderFormat::R8_SINT;
    case ddspp::BC1_TYPELESS:
        return RenderFormat::BC1_TYPELESS;
    case ddspp::BC1_UNORM:
        return RenderFormat::BC1_UNORM;
    case ddspp::BC1_UNORM_SRGB:
        return RenderFormat::BC1_UNORM_SRGB;
    case ddspp::BC2_TYPELESS:
        return RenderFormat::BC2_TYPELESS;
    case ddspp::BC2_UNORM:
        return RenderFormat::BC2_UNORM;
    case ddspp::BC2_UNORM_SRGB:
        return RenderFormat::BC2_UNORM_SRGB;
    case ddspp::BC3_TYPELESS:
        return RenderFormat::BC3_TYPELESS;
    case ddspp::BC3_UNORM:
        return RenderFormat::BC3_UNORM;
    case ddspp::BC3_UNORM_SRGB:
        return RenderFormat::BC3_UNORM_SRGB;
    case ddspp::BC4_TYPELESS:
        return RenderFormat::BC4_TYPELESS;
    case ddspp::BC4_UNORM:
        return RenderFormat::BC4_UNORM;
    case ddspp::BC4_SNORM:
        return RenderFormat::BC4_SNORM;
    case ddspp::BC5_TYPELESS:
        return RenderFormat::BC5_TYPELESS;
    case ddspp::BC5_UNORM:
        return RenderFormat::BC5_UNORM;
    case ddspp::BC5_SNORM:
        return RenderFormat::BC5_SNORM;
    case ddspp::BC6H_TYPELESS:
        return RenderFormat::BC6H_TYPELESS;
    case ddspp::BC6H_UF16:
        return RenderFormat::BC6H_UF16;
    case ddspp::BC6H_SF16:
        return RenderFormat::BC6H_SF16;
    case ddspp::BC7_TYPELESS:
        return RenderFormat::BC7_TYPELESS;
    case ddspp::BC7_UNORM:
        return RenderFormat::BC7_UNORM;
    case ddspp::BC7_UNORM_SRGB:
        return RenderFormat::BC7_UNORM_SRGB;
    case ddspp::B5G6R5_UNORM:
        return RenderFormat::B5G6R5_UNORM;
    case ddspp::B5G5R5A1_UNORM:
        return RenderFormat::B5G5R5A1_UNORM;
    default:
        LOGFN_ERROR("Unsupported format from DDS: {:d}\n", (uint32_t)format);
        __builtin_trap();
        return RenderFormat::UNKNOWN;
    }
}

#ifdef MARATHON_RECOMP_IOS
#include <os/ios/memory_ios.h>
#endif

static uint32_t ComputeMaxValidMipLevels(uint32_t width, uint32_t height, uint32_t depth = 1)
{
    const uint32_t maxDim = std::max({ 1u, width, height, depth });
    return static_cast<uint32_t>(std::bit_width(maxDim));
}

static bool IsBCCompressedFormat(RenderFormat format)
{
    switch (format)
    {
    case RenderFormat::BC1_TYPELESS:
    case RenderFormat::BC1_UNORM:
    case RenderFormat::BC1_UNORM_SRGB:
    case RenderFormat::BC2_TYPELESS:
    case RenderFormat::BC2_UNORM:
    case RenderFormat::BC2_UNORM_SRGB:
    case RenderFormat::BC3_TYPELESS:
    case RenderFormat::BC3_UNORM:
    case RenderFormat::BC3_UNORM_SRGB:
    case RenderFormat::BC4_TYPELESS:
    case RenderFormat::BC4_UNORM:
    case RenderFormat::BC4_SNORM:
    case RenderFormat::BC5_TYPELESS:
    case RenderFormat::BC5_UNORM:
    case RenderFormat::BC5_SNORM:
    case RenderFormat::BC6H_TYPELESS:
    case RenderFormat::BC6H_UF16:
    case RenderFormat::BC6H_SF16:
    case RenderFormat::BC7_TYPELESS:
    case RenderFormat::BC7_UNORM:
    case RenderFormat::BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool DecompressBCSliceToLinear(
    RenderFormat bcFormat,
    const uint8_t* srcBlocks,
    uint32_t srcRowPitch,
    uint32_t width,
    uint32_t height,
    uint8_t* dstLinear,
    uint32_t dstRowPitch)
{
    const uint32_t safeW = std::max(1u, width);
    const uint32_t safeH = std::max(1u, height);
    const uint32_t blocksX = (safeW + 3u) / 4u;
    const uint32_t blocksY = (safeH + 3u) / 4u;

    uint32_t blockBytes = 16;
    uint32_t dstBytesPerPixel = 4;
    switch (bcFormat)
    {
    case RenderFormat::BC1_TYPELESS:
    case RenderFormat::BC1_UNORM:
    case RenderFormat::BC1_UNORM_SRGB:
        blockBytes = BCDEC_BC1_BLOCK_SIZE;
        dstBytesPerPixel = 4;
        break;
    case RenderFormat::BC4_TYPELESS:
    case RenderFormat::BC4_UNORM:
    case RenderFormat::BC4_SNORM:
        blockBytes = BCDEC_BC4_BLOCK_SIZE;
        dstBytesPerPixel = 1;
        break;
    case RenderFormat::BC5_TYPELESS:
    case RenderFormat::BC5_UNORM:
    case RenderFormat::BC5_SNORM:
        blockBytes = BCDEC_BC5_BLOCK_SIZE;
        dstBytesPerPixel = 2;
        break;
    case RenderFormat::BC6H_TYPELESS:
    case RenderFormat::BC6H_UF16:
    case RenderFormat::BC6H_SF16:
        blockBytes = BCDEC_BC6H_BLOCK_SIZE;
        dstBytesPerPixel = 8; // RGBA16F
        break;
    default:
        blockBytes = 16;
        dstBytesPerPixel = 4;
        break;
    }

    alignas(16) uint8_t tileBuf[4 * 4 * 16] = {};

    for (uint32_t by = 0; by < blocksY; ++by)
    {
        const uint8_t* srcRow = srcBlocks + static_cast<size_t>(by) * srcRowPitch;
        for (uint32_t bx = 0; bx < blocksX; ++bx)
        {
            const uint8_t* blockPtr = srcRow + static_cast<size_t>(bx) * blockBytes;
            const uint32_t pxX = bx * 4u;
            const uint32_t pxY = by * 4u;
            const bool fullTile = (pxX + 4u <= safeW) && (pxY + 4u <= safeH);

            switch (bcFormat)
            {
            case RenderFormat::BC1_TYPELESS:
            case RenderFormat::BC1_UNORM:
            case RenderFormat::BC1_UNORM_SRGB:
                if (fullTile)
                    bcdec_bc1(blockPtr, dstLinear + static_cast<size_t>(pxY) * dstRowPitch + pxX * 4u, static_cast<int>(dstRowPitch));
                else
                    bcdec_bc1(blockPtr, tileBuf, 4 * 4);
                break;

            case RenderFormat::BC2_TYPELESS:
            case RenderFormat::BC2_UNORM:
            case RenderFormat::BC2_UNORM_SRGB:
                if (fullTile)
                    bcdec_bc2(blockPtr, dstLinear + static_cast<size_t>(pxY) * dstRowPitch + pxX * 4u, static_cast<int>(dstRowPitch));
                else
                    bcdec_bc2(blockPtr, tileBuf, 4 * 4);
                break;

            case RenderFormat::BC3_TYPELESS:
            case RenderFormat::BC3_UNORM:
            case RenderFormat::BC3_UNORM_SRGB:
                if (fullTile)
                    bcdec_bc3(blockPtr, dstLinear + static_cast<size_t>(pxY) * dstRowPitch + pxX * 4u, static_cast<int>(dstRowPitch));
                else
                    bcdec_bc3(blockPtr, tileBuf, 4 * 4);
                break;

            case RenderFormat::BC4_TYPELESS:
            case RenderFormat::BC4_UNORM:
            case RenderFormat::BC4_SNORM:
                if (fullTile)
                    bcdec_bc4(blockPtr, dstLinear + static_cast<size_t>(pxY) * dstRowPitch + pxX * 1u, static_cast<int>(dstRowPitch));
                else
                    bcdec_bc4(blockPtr, tileBuf, 4 * 1);
                break;

            case RenderFormat::BC5_TYPELESS:
            case RenderFormat::BC5_UNORM:
            case RenderFormat::BC5_SNORM:
                if (fullTile)
                    bcdec_bc5(blockPtr, dstLinear + static_cast<size_t>(pxY) * dstRowPitch + pxX * 2u, static_cast<int>(dstRowPitch));
                else
                    bcdec_bc5(blockPtr, tileBuf, 4 * 2);
                break;

            case RenderFormat::BC6H_TYPELESS:
            case RenderFormat::BC6H_UF16:
            case RenderFormat::BC6H_SF16:
            {
                // bcdec_bc6h_half outputs 3x uint16_t RGB per pixel; expand to RGBA16F (alpha = 1.0 half = 0x3C00)
                uint16_t rgbHalf[4 * 4 * 3] = {};
                const int isSigned = (bcFormat == RenderFormat::BC6H_SF16) ? 1 : 0;
                bcdec_bc6h_half(blockPtr, rgbHalf, 4 * 3, isSigned);
                uint16_t* rgbaHalf = reinterpret_cast<uint16_t*>(tileBuf);
                for (uint32_t i = 0; i < 16; ++i)
                {
                    rgbaHalf[i * 4 + 0] = rgbHalf[i * 3 + 0];
                    rgbaHalf[i * 4 + 1] = rgbHalf[i * 3 + 1];
                    rgbaHalf[i * 4 + 2] = rgbHalf[i * 3 + 2];
                    rgbaHalf[i * 4 + 3] = 0x3C00u;
                }
                const uint32_t copyW = std::min(4u, safeW - pxX);
                const uint32_t copyH = std::min(4u, safeH - pxY);
                for (uint32_t r = 0; r < copyH; ++r)
                {
                    std::memcpy(
                        dstLinear + static_cast<size_t>(pxY + r) * dstRowPitch + static_cast<size_t>(pxX) * 8u,
                        tileBuf + static_cast<size_t>(r) * 32u,
                        static_cast<size_t>(copyW) * 8u);
                }
                continue;
            }

            case RenderFormat::BC7_TYPELESS:
            case RenderFormat::BC7_UNORM:
            case RenderFormat::BC7_UNORM_SRGB:
                if (fullTile)
                    bcdec_bc7(blockPtr, dstLinear + static_cast<size_t>(pxY) * dstRowPitch + pxX * 4u, static_cast<int>(dstRowPitch));
                else
                    bcdec_bc7(blockPtr, tileBuf, 4 * 4);
                break;

            default:
                return false;
            }

            if (!fullTile)
            {
                const uint32_t copyW = std::min(4u, safeW - pxX);
                const uint32_t copyH = std::min(4u, safeH - pxY);
                const uint32_t tileRowBytes = 4u * dstBytesPerPixel;
                for (uint32_t r = 0; r < copyH; ++r)
                {
                    std::memcpy(
                        dstLinear + static_cast<size_t>(pxY + r) * dstRowPitch + static_cast<size_t>(pxX) * dstBytesPerPixel,
                        tileBuf + static_cast<size_t>(r) * tileRowBytes,
                        static_cast<size_t>(copyW) * dstBytesPerPixel);
                }
            }
        }
    }

    return true;
}

static RenderFormat MapDecompressedBCFormat(RenderFormat bcFormat)
{
    switch (bcFormat)
    {
    case RenderFormat::BC4_TYPELESS:
    case RenderFormat::BC4_UNORM:
    case RenderFormat::BC4_SNORM:
        return RenderFormat::R8_UNORM;
    case RenderFormat::BC5_TYPELESS:
    case RenderFormat::BC5_UNORM:
    case RenderFormat::BC5_SNORM:
        return RenderFormat::R8G8_UNORM;
    case RenderFormat::BC6H_TYPELESS:
    case RenderFormat::BC6H_UF16:
    case RenderFormat::BC6H_SF16:
        return RenderFormat::R16G16B16A16_FLOAT;
    default:
        return RenderFormat::R8G8B8A8_UNORM;
    }
}

static bool LoadTexture(GuestTexture& texture, const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping, bool allowMipClamp = false)
{
    if (data == nullptr || dataSize == 0)
        return false;

    ddspp::Descriptor ddsDesc;
    if (dataSize >= sizeof(uint32_t) + sizeof(ddspp::Header) &&
        ddspp::decode_header((unsigned char *)(data), ddsDesc) != ddspp::Error)
    {
        const uint32_t baseWidth = std::max(1u, ddsDesc.width);
        const uint32_t baseHeight = std::max(1u, ddsDesc.height);
        const uint32_t baseDepth = std::max(1u, ddsDesc.depth);
        const uint32_t maxPossibleMips = ComputeMaxValidMipLevels(baseWidth, baseHeight, baseDepth);
        const uint32_t safeNumMips = std::clamp(ddsDesc.numMips, 1u, maxPossibleMips);

        uint32_t startMip = 0;
#ifdef MARATHON_RECOMP_IOS
        // Apply Low4GB / Jetsam mip-0 skipping strictly to multi-mip game DDS textures (allowMipClamp && safeNumMips > 1),
        // never to single-mip UI / ImGui textures loaded in InitImGuiUtils().
        if (allowMipClamp && safeNumMips > 1 && ddsDesc.type == ddspp::Texture2D && (baseWidth >= 512 || baseHeight >= 512))
        {
            const auto& budget = os::ios::GetMemoryBudget();
            const uint64_t liveBytes = g_liveTextureBytes.load(std::memory_order_relaxed);
            const bool overSoftCap = liveBytes >= (budget.texturePoolMaxBytes * 3) / 4;
            const bool largeOn4GB = (budget.tier == os::ios::DeviceMemoryTier::Low4GB) && (baseWidth >= 1024 || baseHeight >= 1024);

            if (largeOn4GB || overSoftCap || os::ios::IsJetsamPressureHigh())
                startMip = 1;
        }
#else
        (void)allowMipClamp;
#endif

        const RenderFormat rawFormat = ConvertDXGIFormat(ddsDesc.format);
        bool decompressBCOnCPU = false;
#ifdef MARATHON_RECOMP_IOS
        if (IsBCCompressedFormat(rawFormat) && !os::ios::SupportsNativeBCTextures())
        {
            decompressBCOnCPU = true;
        }
#endif

        RenderTextureDesc desc;
        desc.dimension = ConvertTextureDimension(ddsDesc.type);
        desc.width = std::max(1u, baseWidth >> startMip);
        desc.height = std::max(1u, baseHeight >> startMip);
        desc.depth = std::max(1u, baseDepth >> startMip);
        const uint32_t maxValidMipsForClampedSize = ComputeMaxValidMipLevels(desc.width, desc.height, desc.depth);
        desc.mipLevels = std::clamp(safeNumMips - startMip, 1u, maxValidMipsForClampedSize);
        const uint32_t baseArraySize = std::max(1u, ddsDesc.arraySize);
        desc.arraySize = ddsDesc.type == ddspp::TextureType::Cubemap ? baseArraySize * 6u : baseArraySize;
        desc.format = decompressBCOnCPU ? MapDecompressedBCFormat(rawFormat) : rawFormat;
        desc.flags = ddsDesc.type == ddspp::TextureType::Cubemap ? RenderTextureFlag::CUBE : RenderTextureFlag::NONE;

        RenderTextureViewDesc viewDesc;
        viewDesc.format = desc.format;
        viewDesc.dimension = ConvertTextureViewDimension(ddsDesc.type);
        viewDesc.mipLevels = desc.mipLevels;

        if (ddsDesc.format == ddspp::A8_UNORM)
        {
            // Map A8_UNORM to R8_UNORM for compatability
            componentMapping = RenderComponentMapping(RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::R);
        }

        viewDesc.componentMapping = componentMapping;

        texture.type = ResourceType::Texture;
        texture.textureHolder = Video::GetRenderDevice()->createTexture(desc);
        if (!texture.textureHolder)
        {
            LOGFN_ERROR("Failed to create host texture ({}x{}, mips={}, fmt={})", desc.width, desc.height, desc.mipLevels, static_cast<uint32_t>(desc.format));
            return false;
        }
        texture.texture = texture.textureHolder.get();
        texture.layout = RenderTextureLayout::COPY_DEST;
        texture.textureView = texture.texture->createTextureView(viewDesc);
        texture.descriptorIndex = g_textureDescriptorAllocator.allocate();
        g_textureDescriptorSet->setTexture(texture.descriptorIndex, texture.texture, RenderTextureLayout::SHADER_READ, texture.textureView.get());

        texture.width = desc.width;
        texture.height = desc.height;
        texture.depth = desc.depth;
        texture.format = desc.format;
        texture.mipLevels = viewDesc.mipLevels;
        texture.viewDimension = viewDesc.dimension;

        struct Slice
        {
            uint32_t width;
            uint32_t height;
            uint32_t depth;
            uint32_t srcOffset;
            uint32_t dstOffset;
            uint32_t srcRowPitch;
            uint32_t dstRowPitch;
            uint32_t rowCount;
            uint32_t dstRowWidthPixels;
        };

        std::vector<Slice> slices;
        uint32_t curSrcOffset = 0;
        uint32_t curDstOffset = 0;
        const uint32_t dstPixelBytes = RenderFormatSize(desc.format);

        for (uint32_t arraySlice = 0; arraySlice < desc.arraySize; arraySlice++)
        {
            for (uint32_t mipSlice = 0; mipSlice < ddsDesc.numMips; mipSlice++)
            {
                const uint32_t sliceWidth = std::max(1u, baseWidth >> mipSlice);
                const uint32_t sliceHeight = std::max(1u, baseHeight >> mipSlice);
                const uint32_t sliceDepth = std::max(1u, baseDepth >> mipSlice);
                const uint32_t rowPitch = ((sliceWidth + ddsDesc.blockWidth - 1) / ddsDesc.blockWidth) * ddsDesc.bitsPerPixelOrBlock;
                const uint32_t srcRowPitch = (rowPitch + 7) / 8;
                const uint32_t srcRowCount = (sliceHeight + ddsDesc.blockHeight - 1) / ddsDesc.blockHeight;
                const uint32_t srcBytes = srcRowPitch * srcRowCount * sliceDepth;

                if (mipSlice >= startMip && (mipSlice - startMip) < desc.mipLevels)
                {
                    auto& slice = slices.emplace_back();
                    slice.width = sliceWidth;
                    slice.height = sliceHeight;
                    slice.depth = sliceDepth;
                    slice.srcOffset = curSrcOffset;
                    slice.dstOffset = curDstOffset;
                    slice.srcRowPitch = srcRowPitch;

                    if (decompressBCOnCPU)
                    {
                        slice.dstRowPitch = (sliceWidth * dstPixelBytes + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
                        slice.rowCount = sliceHeight;
                        slice.dstRowWidthPixels = slice.dstRowPitch / std::max(1u, dstPixelBytes);
                    }
                    else
                    {
                        slice.dstRowPitch = (srcRowPitch + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
                        slice.rowCount = srcRowCount;
                        slice.dstRowWidthPixels = (slice.dstRowPitch * 8) / ddsDesc.bitsPerPixelOrBlock * ddsDesc.blockWidth;
                    }

                    curDstOffset += (slice.dstRowPitch * slice.rowCount * sliceDepth + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);
                }

                curSrcOffset += srcBytes;
            }
        }

        curDstOffset = std::max<uint32_t>(curDstOffset, PLACEMENT_ALIGNMENT);
        texture.allocatedBytes = curDstOffset;
        g_liveTextureBytes.fetch_add(texture.allocatedBytes, std::memory_order_relaxed);

        auto uploadBuffer = Video::GetRenderDevice()->createBuffer(RenderBufferDesc::UploadBuffer(curDstOffset));
        auto mappedMemory = static_cast<uint8_t*>(uploadBuffer->map());

        for (auto& slice : slices)
        {
            if (ddsDesc.headerSize + slice.srcOffset >= dataSize)
                break;

            const uint8_t* srcData = data + ddsDesc.headerSize + slice.srcOffset;
            uint8_t* dstData = mappedMemory + slice.dstOffset;

            if (decompressBCOnCPU)
            {
                const size_t srcSlicePitch = static_cast<size_t>(slice.srcRowPitch) * ((slice.height + ddsDesc.blockHeight - 1) / ddsDesc.blockHeight);
                const size_t dstSlicePitch = static_cast<size_t>(slice.dstRowPitch) * slice.height;
                for (uint32_t z = 0; z < slice.depth; ++z)
                {
                    DecompressBCSliceToLinear(
                        rawFormat,
                        srcData + z * srcSlicePitch,
                        slice.srcRowPitch,
                        slice.width,
                        slice.height,
                        dstData + z * dstSlicePitch,
                        slice.dstRowPitch);
                }
            }
            else if (slice.srcRowPitch == slice.dstRowPitch)
            {
                memcpy(dstData, srcData, slice.srcRowPitch * slice.rowCount * slice.depth);
            }
            else
            {
                for (size_t i = 0; i < slice.rowCount * slice.depth; i++)
                {
                    memcpy(dstData, srcData, slice.srcRowPitch);
                    srcData += slice.srcRowPitch;
                    dstData += slice.dstRowPitch;
                }
            }
        }

        uploadBuffer->unmap();

        Video::ExecuteCopyCommandList([&](RenderCommandList* in_pCommandList)
        {
            in_pCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.texture, RenderTextureLayout::COPY_DEST));

            for (size_t i = 0; i < slices.size(); i++)
            {
                auto& slice = slices[i];

                in_pCommandList->copyTextureRegion(
                    RenderTextureCopyLocation::Subresource(texture.texture, static_cast<uint32_t>(i % desc.mipLevels), static_cast<uint32_t>(i / desc.mipLevels)),
                    RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), desc.format, slice.width, slice.height, slice.depth, slice.dstRowWidthPixels, slice.dstOffset));
            }
        });

        return true;
    }

    int width = 0, height = 0;
    void* stbImage = stbi_load_from_memory(data, static_cast<int>(dataSize), &width, &height, nullptr, 4);

    if (stbImage != nullptr)
    {
        const uint32_t safeW = std::max(1u, static_cast<uint32_t>(std::max(1, width)));
        const uint32_t safeH = std::max(1u, static_cast<uint32_t>(std::max(1, height)));

        texture.type = ResourceType::Texture;
        texture.textureHolder = Video::GetRenderDevice()->createTexture(RenderTextureDesc::Texture2D(safeW, safeH, 1, RenderFormat::R8G8B8A8_UNORM));
        if (!texture.textureHolder)
        {
            stbi_image_free(stbImage);
            return false;
        }
        texture.texture = texture.textureHolder.get();
        texture.width = safeW;
        texture.height = safeH;
        texture.depth = 1;
        texture.mipLevels = 1;
        texture.format = RenderFormat::R8G8B8A8_UNORM;
        texture.viewDimension = RenderTextureViewDimension::TEXTURE_2D;
        texture.layout = RenderTextureLayout::COPY_DEST;

        texture.descriptorIndex = g_textureDescriptorAllocator.allocate();
        g_textureDescriptorSet->setTexture(texture.descriptorIndex, texture.texture, RenderTextureLayout::SHADER_READ);

        uint32_t rowPitch = (safeW * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
        uint32_t slicePitch = rowPitch * safeH;
        texture.allocatedBytes = slicePitch;
        g_liveTextureBytes.fetch_add(texture.allocatedBytes, std::memory_order_relaxed);

        auto uploadBuffer = Video::GetRenderDevice()->createBuffer(RenderBufferDesc::UploadBuffer(slicePitch));
        auto mappedMemory = static_cast<uint8_t*>(uploadBuffer->map());

        if (rowPitch == (safeW * 4))
        {
            memcpy(mappedMemory, stbImage, slicePitch);
        }
        else
        {
            auto srcPtr = static_cast<const uint8_t*>(stbImage);

            for (size_t i = 0; i < safeH; i++)
            {
                memcpy(mappedMemory, srcPtr, safeW * 4);
                srcPtr += safeW * 4;
                mappedMemory += rowPitch;
            }
        }

        uploadBuffer->unmap();

        stbi_image_free(stbImage);

        Video::ExecuteCopyCommandList([&](RenderCommandList* in_pCommandList)
        {
            in_pCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.texture, RenderTextureLayout::COPY_DEST));

            in_pCommandList->copyTextureRegion(
                RenderTextureCopyLocation::Subresource(texture.texture, 0),
                RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), RenderFormat::R8G8B8A8_UNORM, safeW, safeH, 1, rowPitch / 4, 0));
        });

        return true;
    }

    return false;
}

std::unique_ptr<GuestTexture> LoadTexture(const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping)
{
    GuestTexture texture;

    if (LoadTexture(texture, data, dataSize, componentMapping))
        return std::make_unique<GuestTexture>(std::move(texture));

    return nullptr;
}

static void DiffPatchTexture(GuestTexture& texture, uint8_t* data, uint32_t dataSize)
{
#ifdef MARATHON_RECOMP_IOS
    if (os::ios::IsJetsamPressureHigh())
        return;
#endif
    static const std::unique_ptr<uint8_t[]> buttonBcDiff = decompressZstd(g_button_bc_diff, g_button_bc_diff_uncompressed_size);

    auto header = reinterpret_cast<BlockCompressionDiffPatchHeader*>(buttonBcDiff.get());
    auto entries = reinterpret_cast<BlockCompressionDiffPatchEntry*>(buttonBcDiff.get() + header->entriesOffset);
    auto end = entries + header->entryCount;

    auto hash = XXH3_64bits(data, dataSize);

    auto findResult = std::lower_bound(entries, end, hash, [](BlockCompressionDiffPatchEntry& lhs, XXH64_hash_t rhs)
    {
        return lhs.hash < rhs;
    });

    if (findResult != end && findResult->hash == hash)
    {
        auto patch = reinterpret_cast<BlockCompressionDiffPatch*>(buttonBcDiff.get() + findResult->patchesOffset);

        for (size_t i = 0; i < findResult->patchCount; i++)
        {
            assert(patch->destinationOffset + patch->patchBytesSize <= dataSize);
            memcpy(data + patch->destinationOffset, buttonBcDiff.get() + patch->patchBytesOffset, patch->patchBytesSize);
            ++patch;
        }

        GuestTexture patchedTexture;

        if (LoadTexture(patchedTexture, data, dataSize, {}, true))
            texture.patchedTexture = std::make_unique<GuestTexture>(std::move(patchedTexture));
    }
}

static void MakePictureData(Sonicteam::MyTexture* pictureData, uint8_t* data, uint32_t dataSize)
{
    if (data != nullptr)
    {
        GuestTexture texture;

        if (LoadTexture(texture, data, dataSize, {}, true))
        {
#ifdef _DEBUG
            if (pictureData->m_MgrResourceName.size() > 0)
                texture.texture->setName(fmt::format("Texture {}", pictureData->m_MgrResourceName.c_str()));
#endif
            DiffPatchTexture(texture, data, dataSize);

            pictureData->m_pTexture = (void*)g_memory.MapVirtual(g_userHeap.AllocPhysical<GuestTexture>(std::move(texture)));
            pictureData->m_Width = texture.width;
            pictureData->m_Height = texture.height;
        }
    }
}

GUEST_FUNCTION_HOOK(sub_82656B68, MakePictureData);
GUEST_FUNCTION_HOOK(sub_82656DB8, MakePictureData);
