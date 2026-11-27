#include <stdafx.h>
#include "watchdog.h"
#include "host_thread.h"

#include <app.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <gpu/video.h>
#include <kernel/memory.h>
#include <xbox.h>
#include <mutex>
#include <os/logger.h>
#include <thread>
#include <user/paths.h>

#ifdef MARATHON_RECOMP_IOS
#include <os/ios/memory_ios.h>
#endif

namespace
{
    constexpr size_t MAX_WATCHED_THREADS = 64;
    constexpr uint64_t STALL_THRESHOLD_MS = 3000;
    constexpr uint64_t POLL_INTERVAL_MS = 250;
    constexpr uint64_t REPEAT_LOG_INTERVAL_MS = 5000;

    uint64_t NowSteadyMs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    const char* ThreadKindToString(Watchdog::ThreadKind kind) noexcept
    {
        switch (kind)
        {
        case Watchdog::ThreadKind::GuestMain:
            return "GuestMain";
        case Watchdog::ThreadKind::GuestWorker:
            return "GuestWorker";
        case Watchdog::ThreadKind::Render:
            return "RenderThread";
        case Watchdog::ThreadKind::Audio:
            return "AudioThread";
        default:
            return "Unknown";
        }
    }

    const char* WaitObjectTypeToString(uint32_t objType) noexcept
    {
        switch (objType)
        {
        case 0:
            return "NotificationEvent(ManualReset)";
        case 1:
            return "SynchronizationEvent(AutoReset)";
        case 5:
            return "Semaphore";
        case 0x100:
            return "CriticalSection";
        case 0x101:
            return "KernelHandle";
        case 0x102:
            return "MultipleObjects";
        case 0x103:
            return "GpuSync/SwapChain";
        case 0x104:
            return "ThreadSuspend";
        default:
            return "None/Other";
        }
    }

    uint32_t ResolveNearestGuestFunction(uint32_t guestAddress, uint32_t& outOffset) noexcept
    {
        outOffset = 0;
        if (guestAddress < PPC_CODE_BASE || guestAddress >= (PPC_CODE_BASE + PPC_CODE_SIZE))
            return 0;

        uint32_t bestStart = 0;
        for (size_t i = 0; PPCFuncMappings[i].guest != 0; ++i)
        {
            const uint32_t fnAddr = static_cast<uint32_t>(PPCFuncMappings[i].guest);
            if (fnAddr <= guestAddress && fnAddr > bestStart)
                bestStart = fnAddr;
        }

        if (bestStart != 0)
            outOffset = guestAddress - bestStart;

        return bestStart;
    }

    struct WatchedSlot
    {
        std::atomic<bool> active{ false };
        Watchdog::ThreadKind kind{ Watchdog::ThreadKind::GuestWorker };
        uint32_t hostThreadId{ 0 };
        uint32_t entryGuestAddress{ 0 };
        char name[64]{};

        std::atomic<const PPCContext*> ppcCtx{ nullptr };

        std::atomic<uint64_t> lastProgressMs{ 0 };
        std::atomic<uint64_t> waitEnterMs{ 0 };
        std::atomic<uint64_t> lastReportedMs{ 0 };
        std::atomic<bool> inWait{ false };

        std::atomic<uint32_t> lastGuestLr{ 0 };
        std::atomic<uint32_t> lastGuestCtr{ 0 };
        std::atomic<uint32_t> waitTarget{ 0 };
        std::atomic<uint32_t> waitTimeoutMs{ 0 };
        std::atomic<uint32_t> waitObjectType{ 0xFFFFFFFFu };
        std::atomic<const char*> waitCallName{ nullptr };
        std::atomic<uint64_t> heartbeatCount{ 0 };
    };

    WatchedSlot g_slots[MAX_WATCHED_THREADS];
    thread_local WatchedSlot* t_slot = nullptr;

    std::atomic<uint64_t> g_lastPresentMs{ 0 };
    std::atomic<uint64_t> g_presentCount{ 0 };
    std::atomic<uint32_t> g_lastPresentCallerLr{ 0 };
    std::atomic<uint64_t> g_lastPresentStallReportMs{ 0 };

    std::atomic<bool> g_watchdogRunning{ false };
    std::thread g_watchdogThread;
    std::mutex g_logWriteMutex;

    void WriteAndFlushFile(const std::filesystem::path& path, const char* mode, const char* data, size_t len)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (FILE* fp = std::fopen(path.string().c_str(), mode))
        {
            std::fwrite(data, 1, len, fp);
            std::fflush(fp);
            std::fclose(fp);
        }
    }

    void AppendFreezeLogReport(const std::string& report)
    {
        std::lock_guard lock(g_logWriteMutex);

        const std::filesystem::path gameDir = GetGamePath();
        const std::filesystem::path primaryLog = gameDir / "freeze_log.txt";
        WriteAndFlushFile(primaryLog, "ab", report.data(), report.size());

        // If GetGamePath() resolved to Documents/iReCompiled06/game or a subdirectory, mirror to parent (Documents/iReCompiled06/freeze_log.txt) as well so Files.app always sees it at top-level.
        if (gameDir.filename() == "game")
        {
            const std::filesystem::path parentLog = gameDir.parent_path() / "freeze_log.txt";
            WriteAndFlushFile(parentLog, "ab", report.data(), report.size());
        }

        LOGFN_WARNING("Watchdog stall detected -> written to {}", primaryLog.string());
    }

    void WatchdogLoop()
    {
        while (g_watchdogRunning.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            if (!g_watchdogRunning.load(std::memory_order_relaxed))
                break;

            const uint64_t nowMs = NowSteadyMs();
            bool triggerDump = false;
            size_t triggeredSlotIndex = MAX_WATCHED_THREADS;
            const char* triggerReason = "ThreadStall";

            // 1. Primary detector: Video::Present frame progression stall >= 3000 ms (once at least 1 frame was presented).
            const uint64_t totalPresents = g_presentCount.load(std::memory_order_relaxed);
            const uint64_t lastPresent = g_lastPresentMs.load(std::memory_order_relaxed);
            const uint64_t presentIdleMs = (totalPresents > 0 && lastPresent != 0 && nowMs > lastPresent)
                ? (nowMs - lastPresent)
                : 0;

            if (presentIdleMs >= STALL_THRESHOLD_MS)
            {
                const uint64_t lastRep = g_lastPresentStallReportMs.load(std::memory_order_relaxed);
                if (lastRep == 0 || (nowMs > lastRep && (nowMs - lastRep) >= REPEAT_LOG_INTERVAL_MS))
                {
                    g_lastPresentStallReportMs.store(nowMs, std::memory_order_relaxed);
                    triggerDump = true;
                    triggerReason = "Video::Present frame progression stalled >= 3000 ms";
                }
            }

            // 2. Secondary detector: GuestMain, RenderThread, or AudioThread stalled >= 3000 ms (even before first Present).
            if (!triggerDump)
            {
                for (size_t i = 0; i < MAX_WATCHED_THREADS; ++i)
                {
                    auto& slot = g_slots[i];
                    if (!slot.active.load(std::memory_order_acquire))
                        continue;

                    const bool isWaiting = slot.inWait.load(std::memory_order_relaxed);
                    const uint64_t anchorMs = isWaiting
                        ? slot.waitEnterMs.load(std::memory_order_relaxed)
                        : slot.lastProgressMs.load(std::memory_order_relaxed);

                    if (anchorMs == 0 || nowMs <= anchorMs)
                        continue;

                    const uint64_t elapsedMs = nowMs - anchorMs;
                    if (elapsedMs < STALL_THRESHOLD_MS)
                        continue;

                    if (slot.kind != Watchdog::ThreadKind::GuestMain &&
                        slot.kind != Watchdog::ThreadKind::Render &&
                        slot.kind != Watchdog::ThreadKind::Audio)
                    {
                        continue;
                    }

                    const uint64_t lastRep = slot.lastReportedMs.load(std::memory_order_relaxed);
                    if (lastRep != 0 && nowMs > lastRep && (nowMs - lastRep) < REPEAT_LOG_INTERVAL_MS)
                        continue;

                    slot.lastReportedMs.store(nowMs, std::memory_order_relaxed);
                    triggerDump = true;
                    triggeredSlotIndex = i;
                    triggerReason = "Core engine thread stalled >= 3000 ms";
                    break;
                }
            }

            if (!triggerDump)
                continue;

            std::time_t wallNow = std::time(nullptr);
            char wallBuf[64]{};
            std::tm tmBuf{};
#ifdef _WIN32
            localtime_s(&tmBuf, &wallNow);
#else
            localtime_r(&wallNow, &tmBuf);
#endif
            std::strftime(wallBuf, sizeof(wallBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);

            uint64_t liveTexMiB = g_liveTextureBytes.load(std::memory_order_relaxed) / (1024ULL * 1024ULL);
            uint64_t jetsamAvailMiB = 0;
#ifdef MARATHON_RECOMP_IOS
            const uint64_t rawAvail = os::ios::GetAvailableJetsamMemoryBytes();
            jetsamAvailMiB = (rawAvail == UINT64_MAX) ? 0 : (rawAvail / (1024ULL * 1024ULL));
#endif

            const uint32_t lastPresLr = g_lastPresentCallerLr.load(std::memory_order_relaxed);
            std::string report = fmt::format(
                "============================================================\n"
                "[FREEZE WATCHDOG] {} | reason: {}\n"
                "Present: frames={} | last_present={} ms ago | last_present_lr=0x{:08X}\n"
                "App::s_time={:.3f}s | s_isLoading={} | s_isPaused={} | liveTex={} MiB | jetsamAvail={} MiB\n",
                wallBuf,
                triggerReason,
                totalPresents,
                presentIdleMs,
                lastPresLr,
                App::s_time,
                App::s_isLoading ? 1 : 0,
                App::s_isPaused ? 1 : 0,
                liveTexMiB,
                jetsamAvailMiB);

            for (size_t i = 0; i < MAX_WATCHED_THREADS; ++i)
            {
                auto& slot = g_slots[i];
                if (!slot.active.load(std::memory_order_acquire))
                    continue;

                const bool waiting = slot.inWait.load(std::memory_order_relaxed);
                const uint64_t lastMs = slot.lastProgressMs.load(std::memory_order_relaxed);
                const uint64_t waitMs = slot.waitEnterMs.load(std::memory_order_relaxed);
                const uint64_t idleDuration = nowMs > lastMs ? (nowMs - lastMs) : 0;
                const uint64_t waitDuration = (waiting && nowMs > waitMs) ? (nowMs - waitMs) : 0;

                uint32_t liveLr = slot.lastGuestLr.load(std::memory_order_relaxed);
                uint32_t liveCtr = slot.lastGuestCtr.load(std::memory_order_relaxed);
                uint32_t liveR1 = 0;
                uint32_t liveR3 = 0;
                if (const PPCContext* ctx = slot.ppcCtx.load(std::memory_order_acquire))
                {
                    const uint32_t ctxLr = static_cast<uint32_t>(ctx->lr);
                    const uint32_t ctxCtr = ctx->ctr.u32;
                    if (ctxLr != 0)
                        liveLr = ctxLr;
                    if (ctxCtr != 0)
                        liveCtr = ctxCtr;
                    liveR1 = ctx->r1.u32;
                    liveR3 = ctx->r3.u32;
                }

                uint32_t lrOffset = 0;
                const uint32_t lrFnStart = ResolveNearestGuestFunction(liveLr, lrOffset);
                uint32_t ctrOffset = 0;
                const uint32_t ctrFnStart = ResolveNearestGuestFunction(liveCtr, ctrOffset);

                const char* waitCall = slot.waitCallName.load(std::memory_order_relaxed);
                const uint32_t waitObj = slot.waitTarget.load(std::memory_order_relaxed);
                const uint32_t waitObjType = slot.waitObjectType.load(std::memory_order_relaxed);
                const uint32_t timeoutMs = slot.waitTimeoutMs.load(std::memory_order_relaxed);
                const uint64_t beats = slot.heartbeatCount.load(std::memory_order_relaxed);

                uint32_t csOwner = 0;
                int32_t csRecursion = 0;
                if (waiting && waitObjType == 0x100 && g_memory.base != nullptr && waitObj >= 0x10000u)
                {
                    const auto* cs = reinterpret_cast<const XRTL_CRITICAL_SECTION*>(g_memory.base + waitObj);
                    csOwner = cs->OwningThread.get();
                    csRecursion = cs->RecursionCount.get();
                }

                report += fmt::format(
                    "  {}[{}] \"{}\" (tid=0x{:X}, entry=0x{:08X}, beats={}):\n"
                    "    last_progress={} ms ago | lr=0x{:08X} (sub_{:08X}+0x{:X}) | ctr=0x{:08X} (sub_{:08X}+0x{:X}) | sp=0x{:08X} | r3=0x{:08X}\n",
                    (i == triggeredSlotIndex || (slot.kind == Watchdog::ThreadKind::GuestMain && presentIdleMs >= STALL_THRESHOLD_MS))
                        ? ">>> STALLED "
                        : "    THREAD  ",
                    ThreadKindToString(slot.kind),
                    slot.name,
                    slot.hostThreadId,
                    slot.entryGuestAddress,
                    beats,
                    idleDuration,
                    liveLr,
                    lrFnStart,
                    lrOffset,
                    liveCtr,
                    ctrFnStart,
                    ctrOffset,
                    liveR1,
                    liveR3);

                if (waiting)
                {
                    report += fmt::format(
                        "    WAITING in {} for {} ms | object=0x{:08X} | type={} (0x{:X}) | timeout=0x{:X} ms",
                        waitCall != nullptr ? waitCall : "<unknown>",
                        waitDuration,
                        waitObj,
                        WaitObjectTypeToString(waitObjType),
                        waitObjType,
                        timeoutMs);

                    if (waitObjType == 0x100)
                        report += fmt::format(" | cs_owner_pcr=0x{:08X} recursion={}", csOwner, csRecursion);

                    report += "\n";
                }
                else
                {
                    report += "    RUNNING (not inside tracked host wait; spinning in guest loop or host call)\n";
                }
            }

            AppendFreezeLogReport(report);
        }
    }
}

namespace Watchdog
{
    void Init()
    {
        bool expected = false;
        if (!g_watchdogRunning.compare_exchange_strong(expected, true))
            return;

        g_lastPresentMs.store(NowSteadyMs(), std::memory_order_relaxed);
        g_presentCount.store(0, std::memory_order_relaxed);
        g_lastPresentCallerLr.store(0, std::memory_order_relaxed);
        g_lastPresentStallReportMs.store(0, std::memory_order_relaxed);

        {
            std::lock_guard lock(g_logWriteMutex);
            const char* banner = "=== iReCompiled06 FreezeDetector / Watchdog Session Started ===\n";
            const std::filesystem::path gameDir = GetGamePath();
            WriteAndFlushFile(gameDir / "freeze_log.txt", "wb", banner, std::strlen(banner));
            if (gameDir.filename() == "game")
                WriteAndFlushFile(gameDir.parent_path() / "freeze_log.txt", "wb", banner, std::strlen(banner));
        }

        g_watchdogThread = std::thread(WatchdogLoop);
    }

    void Shutdown()
    {
        if (!g_watchdogRunning.exchange(false))
            return;

        if (g_watchdogThread.joinable())
            g_watchdogThread.join();
    }

    void RegisterCurrentThread(ThreadKind kind, const char* name, uint32_t entryGuestAddress)
    {
        if (t_slot != nullptr)
            return;

        for (auto& slot : g_slots)
        {
            bool expected = false;
            if (slot.active.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                slot.kind = kind;
                slot.hostThreadId = HostThread::GetCurrentID();
                slot.entryGuestAddress = entryGuestAddress;
                std::snprintf(slot.name, sizeof(slot.name), "%s", name != nullptr ? name : "Thread");

                const uint64_t nowMs = NowSteadyMs();
                slot.ppcCtx.store(nullptr, std::memory_order_relaxed);
                slot.lastProgressMs.store(nowMs, std::memory_order_relaxed);
                slot.waitEnterMs.store(0, std::memory_order_relaxed);
                slot.lastReportedMs.store(0, std::memory_order_relaxed);
                slot.inWait.store(false, std::memory_order_relaxed);
                slot.lastGuestLr.store(entryGuestAddress, std::memory_order_relaxed);
                slot.lastGuestCtr.store(0, std::memory_order_relaxed);
                slot.waitTarget.store(0, std::memory_order_relaxed);
                slot.waitTimeoutMs.store(0, std::memory_order_relaxed);
                slot.waitObjectType.store(0xFFFFFFFFu, std::memory_order_relaxed);
                slot.waitCallName.store(nullptr, std::memory_order_relaxed);
                slot.heartbeatCount.store(0, std::memory_order_relaxed);

                t_slot = &slot;
                return;
            }
        }
    }

    void AttachCurrentThreadPPCContext(const void* ppcContextPtr)
    {
        if (t_slot == nullptr)
            return;

        t_slot->ppcCtx.store(static_cast<const PPCContext*>(ppcContextPtr), std::memory_order_release);
    }

    void UnregisterCurrentThread()
    {
        if (t_slot == nullptr)
            return;

        t_slot->ppcCtx.store(nullptr, std::memory_order_release);
        t_slot->inWait.store(false, std::memory_order_relaxed);
        t_slot->waitCallName.store(nullptr, std::memory_order_relaxed);
        t_slot->active.store(false, std::memory_order_release);
        t_slot = nullptr;
    }

    void Heartbeat(uint32_t currentGuestLr)
    {
        if (t_slot == nullptr)
            return;

        const uint64_t nowMs = NowSteadyMs();
        t_slot->lastProgressMs.store(nowMs, std::memory_order_relaxed);
        t_slot->lastReportedMs.store(0, std::memory_order_relaxed);
        t_slot->heartbeatCount.fetch_add(1, std::memory_order_relaxed);
        if (currentGuestLr != 0)
            t_slot->lastGuestLr.store(currentGuestLr, std::memory_order_relaxed);
        if (const PPCContext* ctx = t_slot->ppcCtx.load(std::memory_order_relaxed))
        {
            if (ctx->ctr.u32 != 0)
                t_slot->lastGuestCtr.store(ctx->ctr.u32, std::memory_order_relaxed);
        }
    }

    void NotifyFramePresented(uint32_t callerGuestLr)
    {
        const uint64_t nowMs = NowSteadyMs();
        g_lastPresentMs.store(nowMs, std::memory_order_relaxed);
        g_lastPresentStallReportMs.store(0, std::memory_order_relaxed);
        if (callerGuestLr != 0)
            g_lastPresentCallerLr.store(callerGuestLr, std::memory_order_relaxed);
        g_presentCount.fetch_add(1, std::memory_order_relaxed);
        Heartbeat(callerGuestLr);
    }

    void BeginWait(const char* waitCallName, uint32_t guestCallerLr, uint32_t waitTarget, uint32_t timeoutMs, uint32_t waitObjectType)
    {
        if (t_slot == nullptr)
            return;

        const uint64_t nowMs = NowSteadyMs();
        t_slot->lastProgressMs.store(nowMs, std::memory_order_relaxed);
        t_slot->waitEnterMs.store(nowMs, std::memory_order_relaxed);
        if (guestCallerLr != 0)
            t_slot->lastGuestLr.store(guestCallerLr, std::memory_order_relaxed);
        if (const PPCContext* ctx = t_slot->ppcCtx.load(std::memory_order_relaxed))
        {
            if (ctx->ctr.u32 != 0)
                t_slot->lastGuestCtr.store(ctx->ctr.u32, std::memory_order_relaxed);
        }
        t_slot->waitTarget.store(waitTarget, std::memory_order_relaxed);
        t_slot->waitTimeoutMs.store(timeoutMs, std::memory_order_relaxed);
        t_slot->waitObjectType.store(waitObjectType, std::memory_order_relaxed);
        t_slot->waitCallName.store(waitCallName, std::memory_order_relaxed);
        t_slot->inWait.store(true, std::memory_order_release);
    }

    void EndWait()
    {
        if (t_slot == nullptr)
            return;

        const uint64_t nowMs = NowSteadyMs();
        t_slot->inWait.store(false, std::memory_order_release);
        t_slot->lastProgressMs.store(nowMs, std::memory_order_relaxed);
        t_slot->lastReportedMs.store(0, std::memory_order_relaxed);
    }
}
