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
#include <mutex>
#include <os/logger.h>
#include <thread>
#include <user/paths.h>

#ifdef MARATHON_RECOMP_IOS
#include <os/ios/memory_ios.h>
#endif

namespace
{
    constexpr size_t MAX_WATCHED_THREADS = 32;
    constexpr uint64_t STALL_THRESHOLD_MS = 3000;
    constexpr uint64_t POLL_INTERVAL_MS = 500;
    constexpr uint64_t REPEAT_LOG_INTERVAL_MS = 10000;

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

        std::atomic<uint64_t> lastProgressMs{ 0 };
        std::atomic<uint64_t> waitEnterMs{ 0 };
        std::atomic<uint64_t> lastReportedMs{ 0 };
        std::atomic<bool> inWait{ false };

        std::atomic<uint32_t> lastGuestLr{ 0 };
        std::atomic<uint32_t> waitTarget{ 0 };
        std::atomic<uint32_t> waitTimeoutMs{ 0 };
        std::atomic<const char*> waitCallName{ nullptr };
        std::atomic<uint64_t> heartbeatCount{ 0 };
    };

    WatchedSlot g_slots[MAX_WATCHED_THREADS];
    thread_local WatchedSlot* t_slot = nullptr;

    std::atomic<bool> g_watchdogRunning{ false };
    std::thread g_watchdogThread;
    std::mutex g_logWriteMutex;

    void AppendFreezeLogReport(const std::string& report)
    {
        std::lock_guard lock(g_logWriteMutex);

        std::error_code ec;
        const std::filesystem::path gameDir = GetGamePath();
        std::filesystem::create_directories(gameDir, ec);
        const std::filesystem::path logPath = gameDir / "freeze_log.txt";

        FILE* fp = std::fopen(logPath.string().c_str(), "ab");
        if (fp != nullptr)
        {
            std::fwrite(report.data(), 1, report.size(), fp);
            std::fflush(fp);
            std::fclose(fp);
        }

        LOGFN_WARNING("Watchdog stall detected -> written to {}", logPath.string());
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

                // Guest worker threads legitimately block on INFINITE events/semaphores when idle;
                // trigger primary stall dumps from GuestMain or RenderThread stalls, and include all
                // worker threads in the snapshot so cross-thread deadlocks are completely visible.
                if (slot.kind != Watchdog::ThreadKind::GuestMain &&
                    slot.kind != Watchdog::ThreadKind::Render)
                {
                    continue;
                }

                const uint64_t lastRep = slot.lastReportedMs.load(std::memory_order_relaxed);
                if (lastRep != 0 && nowMs > lastRep && (nowMs - lastRep) < REPEAT_LOG_INTERVAL_MS)
                    continue;

                slot.lastReportedMs.store(nowMs, std::memory_order_relaxed);
                triggerDump = true;
                triggeredSlotIndex = i;
                break;
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

            std::string report = fmt::format(
                "============================================================\n"
                "[FREEZE WATCHDOG] {}\n"
                "App::s_time={:.3f}s | s_isLoading={} | s_isPaused={} | liveTex={} MiB | jetsamAvail={} MiB\n",
                wallBuf,
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

                const uint32_t lr = slot.lastGuestLr.load(std::memory_order_relaxed);
                uint32_t fnOffset = 0;
                const uint32_t fnStart = ResolveNearestGuestFunction(lr, fnOffset);

                const char* waitCall = slot.waitCallName.load(std::memory_order_relaxed);
                const uint32_t waitObj = slot.waitTarget.load(std::memory_order_relaxed);
                const uint32_t timeoutMs = slot.waitTimeoutMs.load(std::memory_order_relaxed);
                const uint64_t beats = slot.heartbeatCount.load(std::memory_order_relaxed);

                report += fmt::format(
                    "  {}[{}] \"{}\" (tid=0x{:X}, entry=0x{:08X}, beats={}):\n"
                    "    last_progress={} ms ago | guest_lr=0x{:08X} (fn=sub_{:08X}+0x{:X})\n",
                    (i == triggeredSlotIndex) ? ">>> STALLED " : "    THREAD  ",
                    ThreadKindToString(slot.kind),
                    slot.name,
                    slot.hostThreadId,
                    slot.entryGuestAddress,
                    beats,
                    idleDuration,
                    lr,
                    fnStart,
                    fnOffset);

                if (waiting)
                {
                    report += fmt::format(
                        "    WAITING in {} for {} ms | object/handle=0x{:08X} | timeout=0x{:X} ms\n",
                        waitCall != nullptr ? waitCall : "<unknown>",
                        waitDuration,
                        waitObj,
                        timeoutMs);
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

        // Clear any stale freeze_log.txt header marker with a fresh session banner.
        {
            std::lock_guard lock(g_logWriteMutex);
            std::error_code ec;
            const std::filesystem::path gameDir = GetGamePath();
            std::filesystem::create_directories(gameDir, ec);
            const std::filesystem::path logPath = gameDir / "freeze_log.txt";
            if (FILE* fp = std::fopen(logPath.string().c_str(), "wb"))
            {
                const char* banner = "=== iReCompiled06 Watchdog Session Started ===\n";
                std::fwrite(banner, 1, std::strlen(banner), fp);
                std::fflush(fp);
                std::fclose(fp);
            }
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
                slot.lastProgressMs.store(nowMs, std::memory_order_relaxed);
                slot.waitEnterMs.store(0, std::memory_order_relaxed);
                slot.lastReportedMs.store(0, std::memory_order_relaxed);
                slot.inWait.store(false, std::memory_order_relaxed);
                slot.lastGuestLr.store(entryGuestAddress, std::memory_order_relaxed);
                slot.waitTarget.store(0, std::memory_order_relaxed);
                slot.waitTimeoutMs.store(0, std::memory_order_relaxed);
                slot.waitCallName.store(nullptr, std::memory_order_relaxed);
                slot.heartbeatCount.store(0, std::memory_order_relaxed);

                t_slot = &slot;
                return;
            }
        }
    }

    void UnregisterCurrentThread()
    {
        if (t_slot == nullptr)
            return;

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
    }

    void BeginWait(const char* waitCallName, uint32_t guestCallerLr, uint32_t waitTarget, uint32_t timeoutMs)
    {
        if (t_slot == nullptr)
            return;

        const uint64_t nowMs = NowSteadyMs();
        t_slot->lastProgressMs.store(nowMs, std::memory_order_relaxed);
        t_slot->waitEnterMs.store(nowMs, std::memory_order_relaxed);
        if (guestCallerLr != 0)
            t_slot->lastGuestLr.store(guestCallerLr, std::memory_order_relaxed);
        t_slot->waitTarget.store(waitTarget, std::memory_order_relaxed);
        t_slot->waitTimeoutMs.store(timeoutMs, std::memory_order_relaxed);
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
