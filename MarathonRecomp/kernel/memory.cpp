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
    // First attempt to map the entire 4 GiB guest window as PROT_READ | PROT_WRITE directly
    // (matching desktop macOS/Linux behavior). Untouched MAP_ANON pages are demand-zeroed by XNU
    // and cost 0 bytes of Jetsam phys_footprint until dirtied, while ensuring fixed Xbox 360 MMIO /
    // GPU aperture reads (such as 0xBE010100 in 0xB8000000..0xC0000000 and 0xC0000000..0xE0000000)
    // never fault with SIGBUS (KERN_PROTECTION_FAILURE).
    void* fullRw = mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (fullRw != nullptr && fullRw != MAP_FAILED)
    {
        base = static_cast<uint8_t*>(fullRw);
        mprotect(base, pageSize, PROT_NONE);
    }
    else
    {
        // Fallback for older XNU configurations where a single 4 GiB writable mapping is rejected:
        // reserve 4 GiB PROT_NONE and commit all valid Xbox 360 regions plus install a SIGBUS/SIGSEGV
        // demand-pager for any remaining guest pages inside [base + pageSize, base + PPC_MEMORY_SIZE).
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
            if (length == 0)
                return true;
            const size_t alignedStart = offset & ~(pageSize - 1);
            const size_t alignedEnd = (offset + length + pageSize - 1) & ~(pageSize - 1);
            return mprotect(base + alignedStart, alignedEnd - alignedStart, PROT_READ | PROT_WRITE) == 0;
        };

        // Try committing the entire [pageSize, PPC_MEMORY_SIZE) window first; if XNU caps total RW VA,
        // commit user heap, XMA/XEX/function table, physical heap, and Xbox 360 GPU/MMIO apertures
        // (0xB8000000..0x100000000, covering 0xBE010100, 0xC0000000..0xE0000000, etc.).
        if (!commitRegion(pageSize, PPC_MEMORY_SIZE - pageSize))
        {
            const size_t userHeapEnd = std::min<size_t>(0x20000ull + budget.guestUserHeapBytes, 0x7FEA0000ull);
            const size_t physHeapEnd = std::min<size_t>(0xA0000000ull + budget.guestPhysicalHeapBytes, PPC_MEMORY_SIZE);

            if (!commitRegion(pageSize, userHeapEnd - pageSize) ||
                !commitRegion(0x7FEA0000ull, 0xA0000000ull - 0x7FEA0000ull) ||
                !commitRegion(0xA0000000ull, physHeapEnd - 0xA0000000ull) ||
                !commitRegion(0xB8000000ull, PPC_MEMORY_SIZE - 0xB8000000ull))
            {
                // Minimal guaranteed fallback including Xbox 360 GPU/MMIO registers at 0xB8000000..0xE0000000
                if (!commitRegion(pageSize, userHeapEnd - pageSize) ||
                    !commitRegion(0x7FEA0000ull, 0xA0000000ull - 0x7FEA0000ull) ||
                    !commitRegion(0xA0000000ull, physHeapEnd - 0xA0000000ull) ||
                    !commitRegion(0xB8000000ull, 0xE0000000ull - 0xB8000000ull))
                {
                    munmap(base, PPC_MEMORY_SIZE);
                    base = nullptr;
                    return;
                }
            }
        }
    }

    os::ios::InstallGuestMemoryFaultHandler(base, PPC_MEMORY_SIZE, pageSize);
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
