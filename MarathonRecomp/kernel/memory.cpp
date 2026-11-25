#include <stdafx.h>
#include "memory.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef MARATHON_RECOMP_IOS
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <os/ios/memory_ios.h>
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#elif defined(MARATHON_RECOMP_IOS)
    const auto& budget = os::ios::GetMemoryBudget();
    const size_t pageSize = budget.osPageSize;

    // On iOS ARM64, 0x0..0x100000000 is __PAGEZERO and 0x100000000+ hosts the Mach-O image.
    // Reserve the 4 GiB guest window with PROT_NONE first so untouched multi-GB regions do not
    // consume XNU's writable VA quota on unentitled 4 GB - 6 GB iPhones.
    void* reserved = mmap(nullptr, PPC_MEMORY_SIZE, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (reserved == MAP_FAILED)
    {
        vm_address_t vmAddr = 0;
        if (vm_allocate(mach_task_self(), &vmAddr, PPC_MEMORY_SIZE, VM_FLAGS_ANYWHERE) == KERN_SUCCESS)
        {
            vm_protect(mach_task_self(), vmAddr, PPC_MEMORY_SIZE, FALSE, VM_PROT_NONE);
            reserved = reinterpret_cast<void*>(vmAddr);
        }
    }

    if (reserved == nullptr || reserved == MAP_FAILED)
    {
        base = nullptr;
        return;
    }

    base = static_cast<uint8_t*>(reserved);

    auto commitRegion = [&](size_t offset, size_t length) -> bool
    {
        const size_t alignedStart = offset & ~(pageSize - 1);
        const size_t alignedEnd = (offset + length + pageSize - 1) & ~(pageSize - 1);
        return mprotect(base + alignedStart, alignedEnd - alignedStart, PROT_READ | PROT_WRITE) == 0;
    };

    // Commit only active Xbox 360 regions, keeping [0, pageSize) and unallocated heap tails PROT_NONE:
    // 1. Low guest region + Jetsam-capped user heap: [pageSize, 0x20000 + guestUserHeapBytes)
    // 2. XMA MMIO + guest XEX image + PPC_LOOKUP_FUNC table: [0x7FEA0000, 0xA0000000)
    // 3. Jetsam-capped physical heap: [0xA0000000, 0xA0000000 + guestPhysicalHeapBytes)
    const size_t userHeapEnd = std::min<size_t>(0x20000ull + budget.guestUserHeapBytes, 0x7FEA0000ull);
    const size_t physHeapEnd = std::min<size_t>(0xA0000000ull + budget.guestPhysicalHeapBytes, PPC_MEMORY_SIZE);

    if (!commitRegion(pageSize, userHeapEnd - pageSize) ||
        !commitRegion(0x7FEA0000ull, 0xA0000000ull - 0x7FEA0000ull) ||
        !commitRegion(0xA0000000ull, physHeapEnd - 0xA0000000ull))
    {
        munmap(base, PPC_MEMORY_SIZE);
        base = nullptr;
        return;
    }
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == nullptr || base == (uint8_t*)MAP_FAILED)
    {
        base = nullptr;
        return;
    }

    const long rawPageSize = sysconf(_SC_PAGESIZE);
    const size_t guardSize = (rawPageSize > 0 && rawPageSize <= 0x10000) ? static_cast<size_t>(rawPageSize) : 4096U;
    mprotect(base, guardSize, PROT_NONE);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
