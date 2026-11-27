#pragma once

#include <cstdint>

namespace Watchdog
{
    enum class ThreadKind : uint8_t
    {
        GuestMain = 0,
        GuestWorker = 1,
        Render = 2,
        Audio = 3
    };

    void Init();
    void Shutdown();

    // Registers the calling thread for 3-second stall monitoring.
    void RegisterCurrentThread(ThreadKind kind, const char* name, uint32_t entryGuestAddress = 0);
    void AttachCurrentThreadPPCContext(const void* ppcContextPtr);
    void UnregisterCurrentThread();

    // Marks forward progress on the calling thread (resets stall timer).
    void Heartbeat(uint32_t currentGuestLr = 0);

    // Marks that Video::Present completed a frame on the host.
    void NotifyFramePresented(uint32_t callerGuestLr = 0);

    // Records entering/leaving a potentially blocking wait or GPU sync call.
    void BeginWait(const char* waitCallName, uint32_t guestCallerLr = 0, uint32_t waitTarget = 0, uint32_t timeoutMs = 0xFFFFFFFFu, uint32_t waitObjectType = 0xFFFFFFFFu);
    void EndWait();

    struct ScopedWait
    {
        ScopedWait(const char* waitCallName, uint32_t guestCallerLr = 0, uint32_t waitTarget = 0, uint32_t timeoutMs = 0xFFFFFFFFu, uint32_t waitObjectType = 0xFFFFFFFFu)
        {
            BeginWait(waitCallName, guestCallerLr, waitTarget, timeoutMs, waitObjectType);
        }

        ~ScopedWait()
        {
            EndWait();
        }

        ScopedWait(const ScopedWait&) = delete;
        ScopedWait& operator=(const ScopedWait&) = delete;
    };
}
