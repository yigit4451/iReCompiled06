#pragma once

#include <cstddef>
#include <cstdint>

namespace os::ios
{
    enum class DeviceMemoryTier : uint8_t
    {
        Low4GB,      // <= 4.5 GiB physical RAM (Jetsam ceiling ~1.85 GiB)
        Mid6GB,      // <= 6.5 GiB physical RAM (Jetsam ceiling ~2.85 GiB)
        High8GBPlus  // >= 8.0 GiB physical RAM (Jetsam ceiling ~4.80+ GiB)
    };

    struct MemoryBudget
    {
        DeviceMemoryTier tier;
        uint64_t physicalMemoryBytes;
        size_t osPageSize;
        size_t guestUserHeapBytes;
        size_t guestPhysicalHeapBytes;
        size_t texturePoolMaxBytes;
        size_t uploadChunkBytes;
        size_t maxRetainedUploadChunks;
        uint32_t maxInternalRenderHeight;
        uint32_t pipelineCompilerThreads;
        uint64_t jetsamLowWaterBytes;
    };

    // Returns the static hardware-tuned memory budget for the active iPhone/iPad.
    const MemoryBudget& GetMemoryBudget();

    // Queries XNU os_proc_available_memory() to report remaining bytes before Jetsam termination.
    uint64_t GetAvailableJetsamMemoryBytes();

    // Returns true when remaining Jetsam headroom drops below the safety watermark.
    bool IsJetsamPressureHigh();

    // Configures MoltenVK environment parameters (MTLHeap suballocation, command buffer caps, compiler concurrency)
    // prior to Vulkan/Metal device creation.
    void ConfigureMoltenVKMemoryPool();

    // Installs UIKit & XNU dispatch memory-pressure observers.
    void InstallMemoryWarningObserver();

    // Atomically consumes and clears any pending iOS memory warning signal.
    bool ConsumeMemoryWarningFlag();

    // Returns true if the active iOS GPU natively supports desktop BC1-BC7 compressed textures
    // (requires Apple GPU Family 9 / A17 Pro / M-series on iOS 16.4+; false on A13/A14/A15/A16).
    bool SupportsNativeBCTextures();

    // Installs SIGBUS + SIGSEGV demand-commit handlers for any PROT_NONE page inside [guestBase + pageSize, guestBase + guestSize)
    // while keeping the null guard page [guestBase, guestBase + pageSize) protected.
    void InstallGuestMemoryFaultHandler(uint8_t* guestBase, size_t guestSize, size_t pageSize);
}
