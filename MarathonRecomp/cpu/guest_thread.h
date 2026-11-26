#pragma once

#include <cstdint>
#include <string>

#include <kernel/xdm.h>
#include <utils/host_thread.h>

#define CURRENT_THREAD_HANDLE uint32_t(-2)

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* thread = nullptr;

    GuestThreadContext(uint32_t cpuNumber);
    ~GuestThreadContext();
};

struct GuestThreadParams
{
    uint32_t function;
    uint32_t value;
    uint32_t flags;
};

struct GuestThreadHandle : KernelObject
{
    GuestThreadParams params;
    std::atomic<bool> suspended;
    HostThread thread;
    // HACK(1)
    std::atomic<bool> isFinished = false;

    GuestThreadHandle(const GuestThreadParams& params);

    uint32_t GetThreadId() const;

    uint32_t Wait(uint32_t timeout) override;
};

// Diagnostics for guest function resolution failures (nullptr in the PPC function table).
struct GuestModuleDiagnostics
{
    uint32_t entryPoint = 0;
    uint32_t imageBase = 0;
    uint32_t imageSize = 0;
    std::string modulePath;
    std::string loaderStatus;
};

void GuestSetModuleDiagnostics(const GuestModuleDiagnostics& diagnostics);

// Builds a human readable report for a guest address that has no host function registered.
std::string GuestBuildMissingFunctionReport(uint32_t guestAddress);

// Records a fatal guest error (logs it and keeps it for the host UI). Thread-safe.
void GuestReportFatalError(const std::string& message);
bool GuestHasFatalError();
std::string GuestGetFatalError();

struct GuestThread
{
    static uint32_t Start(const GuestThreadParams& params);
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();
    static void SetLastError(uint32_t error);
};
