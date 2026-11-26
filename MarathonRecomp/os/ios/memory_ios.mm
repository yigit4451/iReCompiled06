#include <os/ios/memory_ios.h>
#include <os/logger.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <atomic>
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <os/proc.h>
#include <unistd.h>

namespace
{
    constexpr uint64_t MiB = 1024ULL * 1024ULL;
    constexpr uint64_t GiB = 1024ULL * MiB;

    std::atomic<bool> g_memoryWarningPending{ false };
    std::once_flag g_observerOnceFlag;
    dispatch_source_t g_memoryPressureSource = nullptr;

    os::ios::MemoryBudget ComputeMemoryBudget()
    {
        uint64_t physBytes = 4ULL * GiB;
        @autoreleasepool
        {
            physBytes = [[NSProcessInfo processInfo] physicalMemory];
        }

        const long rawPageSize = sysconf(_SC_PAGESIZE);
        const size_t pageSize = (rawPageSize > 0) ? static_cast<size_t>(rawPageSize) : 16384U;

        os::ios::MemoryBudget budget{};
        budget.physicalMemoryBytes = physBytes;
        budget.osPageSize = pageSize;

        if (physBytes <= (4ULL * GiB + 512ULL * MiB))
        {
            // 4 GB RAM iPhones (e.g., iPhone 11 / 12 / 13 / SE): Jetsam limit ~1.85 GiB
            budget.tier = os::ios::DeviceMemoryTier::Low4GB;
            budget.guestUserHeapBytes = 384ULL * MiB;
            budget.guestPhysicalHeapBytes = 384ULL * MiB;
            budget.texturePoolMaxBytes = 384ULL * MiB;
            budget.uploadChunkBytes = 4ULL * MiB;
            budget.maxRetainedUploadChunks = 2;
            budget.maxInternalRenderHeight = 720;
            budget.pipelineCompilerThreads = 1;
            budget.jetsamLowWaterBytes = 192ULL * MiB;
        }
        else if (physBytes <= (6ULL * GiB + 512ULL * MiB))
        {
            // 6 GB RAM iPhones (e.g., iPhone 12 Pro / 13 Pro / 14 / 15): Jetsam limit ~2.85 GiB
            budget.tier = os::ios::DeviceMemoryTier::Mid6GB;
            budget.guestUserHeapBytes = 512ULL * MiB;
            budget.guestPhysicalHeapBytes = 512ULL * MiB;
            budget.texturePoolMaxBytes = 640ULL * MiB;
            budget.uploadChunkBytes = 8ULL * MiB;
            budget.maxRetainedUploadChunks = 2;
            budget.maxInternalRenderHeight = 900;
            budget.pipelineCompilerThreads = 2;
            budget.jetsamLowWaterBytes = 256ULL * MiB;
        }
        else
        {
            // 8 GB+ RAM iPhones / iPads (iPhone 15 Pro / 16+, M-series iPads)
            budget.tier = os::ios::DeviceMemoryTier::High8GBPlus;
            budget.guestUserHeapBytes = 768ULL * MiB;
            budget.guestPhysicalHeapBytes = 768ULL * MiB;
            budget.texturePoolMaxBytes = 1024ULL * MiB;
            budget.uploadChunkBytes = 8ULL * MiB;
            budget.maxRetainedUploadChunks = 3;
            budget.maxInternalRenderHeight = 1080;
            budget.pipelineCompilerThreads = 3;
            budget.jetsamLowWaterBytes = 320ULL * MiB;
        }

        // Align heap sizes to the active XNU page size (16 KiB on Apple Silicon).
        budget.guestUserHeapBytes = (budget.guestUserHeapBytes + pageSize - 1) & ~(pageSize - 1);
        budget.guestPhysicalHeapBytes = (budget.guestPhysicalHeapBytes + pageSize - 1) & ~(pageSize - 1);

        return budget;
    }
}

const os::ios::MemoryBudget& os::ios::GetMemoryBudget()
{
    static const MemoryBudget s_budget = ComputeMemoryBudget();
    return s_budget;
}

uint64_t os::ios::GetAvailableJetsamMemoryBytes()
{
    if (@available(iOS 13.0, *))
    {
        const size_t avail = os_proc_available_memory();
        if (avail > 0)
            return static_cast<uint64_t>(avail);
    }
    return UINT64_MAX;
}

bool os::ios::IsJetsamPressureHigh()
{
    const uint64_t avail = GetAvailableJetsamMemoryBytes();
    if (avail == UINT64_MAX)
        return false;

    return avail < GetMemoryBudget().jetsamLowWaterBytes;
}

void os::ios::ConfigureMoltenVKMemoryPool()
{
    const auto& budget = GetMemoryBudget();

    // Suballocate Vulkan device memory via Metal MTLHeaps instead of page-rounding individual allocations.
    setenv("MVK_CONFIG_USE_MTLHEAP", "1", 0);

    // Avoid pre-allocating heavy Metal command buffer pools in RAM on 4 GB - 6 GB devices.
    setenv("MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS", "0", 0);
    setenv("MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE",
           budget.tier == DeviceMemoryTier::Low4GB ? "12" : "16", 0);

    // Limit concurrent shader compilation XPC spikes on 4 GB - 6 GB iPhones so Jetsam peak stays bounded.
    if (budget.tier != DeviceMemoryTier::High8GBPlus)
        setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "0", 0);

    // Recover gracefully if iOS suspends GPU submissions during app switching.
    setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 0);
}

void os::ios::InstallMemoryWarningObserver()
{
    std::call_once(g_observerOnceFlag, []()
    {
        @autoreleasepool
        {
            [[NSNotificationCenter defaultCenter]
                addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                            object:nil
                             queue:[NSOperationQueue mainQueue]
                        usingBlock:^(__unused NSNotification* note) {
                            g_memoryWarningPending.store(true, std::memory_order_release);
                            LOGN_WARNING("Received iOS UIApplicationDidReceiveMemoryWarningNotification; scheduling GPU/heap trim.");
                        }];
        }

        g_memoryPressureSource = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,
            0,
            DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));

        if (g_memoryPressureSource)
        {
            dispatch_source_set_event_handler(g_memoryPressureSource, ^{
                g_memoryWarningPending.store(true, std::memory_order_release);
            });
            dispatch_resume(g_memoryPressureSource);
        }
    });
}

bool os::ios::ConsumeMemoryWarningFlag()
{
    return g_memoryWarningPending.exchange(false, std::memory_order_acq_rel);
}

bool os::ios::SupportsNativeBCTextures()
{
    static const bool s_supportsBC = []() -> bool
    {
        @autoreleasepool
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device)
                return false;

            bool supported = false;
            if (@available(iOS 16.4, *))
            {
                if ([device respondsToSelector:@selector(supportsBCTextureCompression)])
                {
                    supported = [device supportsBCTextureCompression];
                }
            }
#if !__has_feature(objc_arc)
            [device release];
#endif
            return supported;
        }
    }();
    return s_supportsBC;
}
