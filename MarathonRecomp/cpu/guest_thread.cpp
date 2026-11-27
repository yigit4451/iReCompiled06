#include "guest_thread.h"
#include <stdafx.h>
#include <api/Marathon.h>
#include <fmt/format.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include <os/logger.h>
#include <utils/watchdog.h>
#include "ppc_context.h"
#include <mutex>
#include <atomic>
#include <filesystem>
#ifndef _WIN32
#include <unistd.h>
#endif

constexpr size_t PCR_SIZE = 0xAB0;
constexpr size_t TLS_SIZE = 0x100;
constexpr size_t TEB_SIZE = 0x2E0;
constexpr size_t STACK_SIZE = 0x80000;
constexpr size_t TOTAL_SIZE = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;

constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber)
{
    assert(thread == nullptr);

    thread = (uint8_t*)g_userHeap.Alloc(TOTAL_SIZE);
    memset(thread, 0, TOTAL_SIZE);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    ppcContext.r1.u64 = g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE); // stack pointer
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
    Watchdog::AttachCurrentThreadPPCContext(&ppcContext);
}

GuestThreadContext::~GuestThreadContext()
{
    Watchdog::AttachCurrentThreadPPCContext(nullptr);
    g_userHeap.Free(thread);
}

static std::mutex g_guestDiagMutex;
static GuestModuleDiagnostics g_guestModuleDiag;
static std::string g_guestFatalError;
static std::atomic<bool> g_guestHasFatalError{false};

void GuestSetModuleDiagnostics(const GuestModuleDiagnostics& diagnostics)
{
    std::lock_guard lock(g_guestDiagMutex);
    g_guestModuleDiag = diagnostics;
}

std::string GuestBuildMissingFunctionReport(uint32_t guestAddress)
{
    GuestModuleDiagnostics diag;
    {
        std::lock_guard lock(g_guestDiagMutex);
        diag = g_guestModuleDiag;
    }

    size_t mappingCount = 0;
    bool addressRegisteredInMappings = false;
    uint32_t nearestBelow = 0;
    uint32_t nearestAbove = 0;
    uint32_t lowest = 0xFFFFFFFFu;
    uint32_t highest = 0;

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        const uint32_t guest = static_cast<uint32_t>(PPCFuncMappings[i].guest);
        mappingCount++;

        if (guest == guestAddress)
            addressRegisteredInMappings = true;

        if (guest <= guestAddress && guest > nearestBelow)
            nearestBelow = guest;

        if (guest > guestAddress && (nearestAbove == 0 || guest < nearestAbove))
            nearestAbove = guest;

        lowest = std::min(lowest, guest);
        highest = std::max(highest, guest);
    }

    // A full XenonRecomp build registers tens of thousands of functions and writes PPC_IMAGE_SIZE /
    // PPC_CODE_* from the XEX itself. Public CI builds without MarathonRecompLib/private/default.xex
    // use tools/generate_stub_recomp.py (hook-only table, fixed 0x1200000 / 0x1000000 constants).
    const bool smallTable = mappingCount < 5000;
    const bool stubConstants = diag.imageSize != 0 && diag.imageSize != PPC_IMAGE_SIZE;
    const bool looksLikeStubBuild = smallTable || stubConstants;

    std::string verdict;
    if (g_memory.base == nullptr)
        verdict = "TESHIS: Guest bellek penceresi (g_memory.base) ayrilamadi.";
    else if (guestAddress == 0)
        verdict = "TESHIS: Giris noktasi 0 - default.xex / decrypted/default.bin yuklenemedi.";
    else if (looksLikeStubBuild)
        verdict = "TESHIS: STUB DERLEME - bu IPA'da XenonRecomp ciktisi (oyun kodu) YOK. "
                  "Linker stripping veya Title Update sorunu DEGIL; CI'a ASSET_REPO/ASSET_REPO_TOKEN "
                  "secret'lari ile default.xex + shader.arc + shader_lt.arc eklenmeli.";
    else if (nearestBelow != 0 && (guestAddress - nearestBelow) < 0x4000 && !addressRegisteredInMappings)
        verdict = "TESHIS: Tablo tam ama bu adres yok; muhtemelen XEX revizyonu (Title Update / bolge) uyumsuz "
                  "veya linker bu fonksiyonu atmis.";
    else
        verdict = "TESHIS: Tablo bu XEX'e ait gorunmuyor (revizyon/bolge uyumsuzlugu olasi).";

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);

    // Most important numbers come first: iOS message boxes get truncated.
    std::string report = fmt::format(
        "{}\n"
        "mapping_count={} | yakin_alt=0x{:08X} | yakin_ust=0x{:08X}\n"
        "tablo_aralik=0x{:08X}..0x{:08X} | adres_kayitli={}\n"
        "eksik=0x{:08X} entry=0x{:08X}\n"
        "imaj base/size=0x{:08X}/0x{:X} | PPC_IMAGE_SIZE=0x{:X} PPC_CODE=0x{:X}+0x{:X}\n"
        "xex dosya boyutu={} XXH64={:016X}\n"
        "yukleme: {}\n"
        "modul: {}\n"
        "cwd: {}\n"
        "g_memory.base: {}\n",
        verdict,
        mappingCount, nearestBelow, nearestAbove,
        mappingCount ? lowest : 0u, highest, addressRegisteredInMappings ? "evet" : "hayir",
        guestAddress, diag.entryPoint,
        diag.imageBase, diag.imageSize,
        (unsigned long long)PPC_IMAGE_SIZE, (unsigned long long)PPC_CODE_BASE, (unsigned long long)PPC_CODE_SIZE,
        (unsigned long long)diag.fileSize, (unsigned long long)diag.fileHash,
        diag.loaderStatus.empty() ? "<bilinmiyor>" : diag.loaderStatus,
        diag.modulePath.empty() ? "<yok>" : diag.modulePath,
        ec ? "<alinamadi>" : cwd.string(),
        g_memory.base != nullptr ? "eslenmis" : "NULL");

    return report;
}

void GuestReportFatalError(const std::string& message)
{
    {
        std::lock_guard lock(g_guestDiagMutex);
        if (g_guestFatalError.empty())
            g_guestFatalError = message;
    }
    g_guestHasFatalError.store(true);

    fprintf(stderr, "[iReCompiled06][FATAL] %s\n", message.c_str());
    LOGFN_ERROR("{}", message);
}

bool GuestHasFatalError()
{
    return g_guestHasFatalError.load();
}

std::string GuestGetFatalError()
{
    std::lock_guard lock(g_guestDiagMutex);
    return g_guestFatalError;
}

static std::string GetGuestThreadName(const GuestThreadParams& params)
{
    // Sonicteam::SoX::Thread::ThreadProc
    if (params.function == 0x825866A8) {
        return static_cast<Sonicteam::SoX::Thread*>(g_memory.Translate(params.value))->m_pName.get();
    }
    return fmt::format("Guest Thread (0x{:x})", params.function);
}

static void GuestThreadFunc(void* data)
{
    auto* hThread = static_cast<GuestThreadHandle*>(data);
    const std::string threadName = GetGuestThreadName(hThread->params);
    Watchdog::RegisterCurrentThread(Watchdog::ThreadKind::GuestWorker, threadName.c_str(), hThread->params.function);
    {
        Watchdog::ScopedWait waitGuard("GuestThread::InitialSuspend", hThread->params.function, 0, INFINITE, 0x104u);
        hThread->suspended.wait(true);
    }
    GuestThread::Start(hThread->params);
    Watchdog::UnregisterCurrentThread();
    // HACK(1)
    hThread->isFinished = true;
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params), suspended((params.flags & 0x1) != 0),
      thread(GetGuestThreadName(params), GuestThreadFunc, this)
{
}

template <typename ThreadType>
static uint32_t CalcThreadId(const ThreadType& id)
{
    if constexpr (sizeof(id) == 4)
        return *reinterpret_cast<const uint32_t*>(&id);
    else
        return XXH32(&id, sizeof(id), 0);
}

uint32_t GuestThreadHandle::GetThreadId() const
{
    return CalcThreadId(thread.GetID());
}

uint32_t GuestThreadHandle::Wait(uint32_t timeout)
{
    if (timeout == INFINITE || isFinished.load()) // HACK(1): isFinished
    {
        thread.Join();
        return STATUS_WAIT_0;
    }
    else if (timeout == 0)
    {
        if (thread.IsAlive())
            return STATUS_TIMEOUT;

        return STATUS_WAIT_0;
    }
    else
    {
        auto start = std::chrono::steady_clock::now();
        while (thread.IsAlive())
        {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeout)
                return STATUS_TIMEOUT;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return STATUS_WAIT_0;
    }
}

uint32_t GuestThread::Start(const GuestThreadParams& params)
{
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    // Never jump to a null host function pointer (EXC_BAD_ACCESS at pc 0x0): report why instead.
    const bool inCodeRange = params.function >= PPC_CODE_BASE && params.function <= PPC_CODE_BASE + PPC_CODE_SIZE;
    PPCFunc* hostFunction = (g_memory.base != nullptr && inCodeRange) ? g_memory.FindFunction(params.function) : nullptr;
    if (hostFunction == nullptr)
    {
        GuestReportFatalError(GuestBuildMissingFunctionReport(params.function));
        return 0xFFFFFFFFu;
    }

    GuestThreadContext ctx(cpuNumber);
    ctx.ppcContext.r3.u64 = params.value;

    hostFunction(ctx.ppcContext, g_memory.base);

    return ctx.ppcContext.r3.u32;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto hThread = CreateKernelObject<GuestThreadHandle>(params);

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

uint32_t GuestThread::GetCurrentThreadId()
{
    return CalcThreadId(HostThread::GetCurrentID());
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}

uint32_t XSetThreadProcessor(GuestThreadHandle* hThread, uint32_t dwIdealProcessor)
{
    return 0;
}

GUEST_FUNCTION_HOOK(sub_82537F80, XSetThreadProcessor);
