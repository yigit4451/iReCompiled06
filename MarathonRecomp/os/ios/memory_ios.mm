#include <os/ios/memory_ios.h>
#include <os/logger.h>
#include <hid/hid.h>
#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mutex>
#include <os/proc.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#endif
#include <unistd.h>

namespace
{
    constexpr uint64_t MiB = 1024ULL * 1024ULL;
    constexpr uint64_t GiB = 1024ULL * MiB;

    std::atomic<bool> g_memoryWarningPending{ false };
    std::once_flag g_observerOnceFlag;
    dispatch_source_t g_memoryPressureSource = nullptr;

    std::atomic<uintptr_t> g_guestFaultBase{ 0 };
    std::atomic<size_t> g_guestFaultSize{ 0 };
    std::atomic<size_t> g_guestFaultPageSize{ 16384U };
    std::once_flag g_faultHandlerOnceFlag;
    struct sigaction g_prevSigbusAction{};
    struct sigaction g_prevSigsegvAction{};
    struct sigaction g_prevSigabrtAction{};
    char g_crashLogPath[1024] = {};
    std::atomic_flag g_fatalSignalLogged = ATOMIC_FLAG_INIT;

    void InitCrashLogPathIfNeeded()
    {
        if (g_crashLogPath[0] != '\0')
            return;

        const char* home = std::getenv("HOME");
        if (home != nullptr && home[0] != '\0')
        {
            char dirPath[1024];
            std::snprintf(dirPath, sizeof(dirPath), "%s/Documents/iReCompiled06", home);
            mkdir(dirPath, 0755);
            std::snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s/Documents/iReCompiled06/freeze_log.txt", home);
        }
    }

    const char* GetFatalSignalName(int sig)
    {
        switch (sig)
        {
            case SIGSEGV: return "SIGSEGV";
            case SIGBUS:  return "SIGBUS";
            case SIGABRT: return "SIGABRT";
            default:      return "SIGNAL";
        }
    }

    size_t AppendSignalText(char* buf, size_t cap, size_t pos, const char* text)
    {
        if (text == nullptr)
            return pos;
        while (*text != '\0' && pos + 1 < cap)
            buf[pos++] = *text++;
        return pos;
    }

    size_t AppendSignalUint(char* buf, size_t cap, size_t pos, uint64_t val)
    {
        char tmp[24];
        size_t n = 0;
        do
        {
            tmp[n++] = static_cast<char>('0' + (val % 10ULL));
            val /= 10ULL;
        } while (val > 0 && n < sizeof(tmp));

        while (n > 0 && pos + 1 < cap)
            buf[pos++] = tmp[--n];
        return pos;
    }

    size_t AppendSignalHex(char* buf, size_t cap, size_t pos, uint64_t val)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        pos = AppendSignalText(buf, cap, pos, "0x");
        char tmp[16];
        size_t n = 0;
        do
        {
            tmp[n++] = kHex[val & 0xFULL];
            val >>= 4U;
        } while (val > 0 && n < sizeof(tmp));

        while (n > 0 && pos + 1 < cap)
            buf[pos++] = tmp[--n];
        return pos;
    }

    void WriteFatalSignalFreezeLog(int sig, siginfo_t* info, void* ucontext)
    {
        if (g_fatalSignalLogged.test_and_set(std::memory_order_acq_rel))
            return;

        const uintptr_t crashAddr = (info != nullptr) ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
        const uint64_t availBytes = static_cast<uint64_t>(os_proc_available_memory());

        uint64_t pcVal = 0;
        uint64_t lrVal = 0;
#if defined(__APPLE__) && defined(__aarch64__)
        if (ucontext != nullptr)
        {
            const ucontext_t* uc = static_cast<const ucontext_t*>(ucontext);
            if (uc->uc_mcontext != nullptr)
            {
#if defined(__darwin_arm_thread_state64_get_pc)
                pcVal = static_cast<uint64_t>(__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
                lrVal = static_cast<uint64_t>(__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss));
#else
                pcVal = static_cast<uint64_t>(uc->uc_mcontext->__ss.__pc);
                lrVal = static_cast<uint64_t>(uc->uc_mcontext->__ss.__lr);
#endif
            }
        }
#endif

        char line[320];
        size_t pos = 0;
        pos = AppendSignalText(line, sizeof(line), pos, "[FATAL SIGNAL] sig=");
        pos = AppendSignalUint(line, sizeof(line), pos, static_cast<uint64_t>(sig > 0 ? sig : 0));
        pos = AppendSignalText(line, sizeof(line), pos, " (");
        pos = AppendSignalText(line, sizeof(line), pos, GetFatalSignalName(sig));
        pos = AppendSignalText(line, sizeof(line), pos, ") si_addr=");
        pos = AppendSignalHex(line, sizeof(line), pos, static_cast<uint64_t>(crashAddr));
        pos = AppendSignalText(line, sizeof(line), pos, " pc=");
        pos = AppendSignalHex(line, sizeof(line), pos, pcVal);
        pos = AppendSignalText(line, sizeof(line), pos, " lr=");
        pos = AppendSignalHex(line, sizeof(line), pos, lrVal);
        pos = AppendSignalText(line, sizeof(line), pos, " avail_ram=");
        pos = AppendSignalUint(line, sizeof(line), pos, availBytes / MiB);
        pos = AppendSignalText(line, sizeof(line), pos, " MiB (");
        pos = AppendSignalUint(line, sizeof(line), pos, availBytes);
        pos = AppendSignalText(line, sizeof(line), pos, " bytes)\n");

        (void)write(STDERR_FILENO, line, pos);

        if (g_crashLogPath[0] != '\0')
        {
            const int fd = open(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0)
            {
                (void)write(fd, line, pos);
                close(fd);
            }
        }
    }

    void ForwardUnhandledSignal(int sig, siginfo_t* info, void* ucontext, const struct sigaction& prev)
    {
        if ((prev.sa_flags & SA_SIGINFO) != 0 && prev.sa_sigaction != nullptr &&
            prev.sa_sigaction != reinterpret_cast<void (*)(int, siginfo_t*, void*)>(SIG_IGN) &&
            prev.sa_sigaction != reinterpret_cast<void (*)(int, siginfo_t*, void*)>(SIG_DFL))
        {
            prev.sa_sigaction(sig, info, ucontext);
            return;
        }

        if (prev.sa_handler != nullptr && prev.sa_handler != SIG_IGN && prev.sa_handler != SIG_DFL)
        {
            prev.sa_handler(sig);
            return;
        }

        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, nullptr);
        raise(sig);
    }

    void GuestMemorySignalHandler(int sig, siginfo_t* info, void* ucontext)
    {
        if ((sig == SIGBUS || sig == SIGSEGV) && info != nullptr)
        {
            const uintptr_t base = g_guestFaultBase.load(std::memory_order_relaxed);
            const size_t size = g_guestFaultSize.load(std::memory_order_relaxed);
            const size_t pageSize = g_guestFaultPageSize.load(std::memory_order_relaxed);
            const uintptr_t faultAddr = reinterpret_cast<uintptr_t>(info->si_addr);

            // Commit any PROT_NONE page within [base + pageSize, base + size) on demand;
            // keep page 0 [base, base + pageSize) as a true null-pointer guard.
            if (base != 0 && faultAddr >= (base + pageSize) && faultAddr < (base + size))
            {
                const uintptr_t pageAligned = faultAddr & ~(pageSize - 1);
                // Commit in 64 KiB chunks (bounded to guest end) to amortize sequential MMIO/buffer scans.
                const size_t chunkBytes = std::min<size_t>(pageSize * 4, (base + size) - pageAligned);
                if (mprotect(reinterpret_cast<void*>(pageAligned), chunkBytes, PROT_READ | PROT_WRITE) == 0)
                    return;
            }
        }

        WriteFatalSignalFreezeLog(sig, info, ucontext);

        const struct sigaction& prev = (sig == SIGBUS)
            ? g_prevSigbusAction
            : ((sig == SIGABRT) ? g_prevSigabrtAction : g_prevSigsegvAction);
        ForwardUnhandledSignal(sig, info, ucontext, prev);
    }

    void EnsureSignalHandlersInstalled()
    {
        InitCrashLogPathIfNeeded();
        std::call_once(g_faultHandlerOnceFlag, []()
        {
            struct sigaction act{};
            act.sa_sigaction = GuestMemorySignalHandler;
            sigemptyset(&act.sa_mask);
            act.sa_flags = SA_SIGINFO | SA_NODEFER;

            sigaction(SIGBUS, &act, &g_prevSigbusAction);
            sigaction(SIGSEGV, &act, &g_prevSigsegvAction);
            sigaction(SIGABRT, &act, &g_prevSigabrtAction);
        });
    }

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
            // Note: o1heap rounds fragment allocations up to powers of two; untouched pages cost 0 RSS.
            budget.tier = os::ios::DeviceMemoryTier::Low4GB;
            budget.guestUserHeapBytes = 512ULL * MiB;
            budget.guestPhysicalHeapBytes = 512ULL * MiB;
            budget.texturePoolMaxBytes = 256ULL * MiB;
            budget.uploadChunkBytes = 4ULL * MiB;
            budget.maxRetainedUploadChunks = 1;
            budget.maxInternalRenderHeight = 720;
            budget.pipelineCompilerThreads = 1;
            budget.jetsamLowWaterBytes = 256ULL * MiB;
        }
        else if (physBytes <= (6ULL * GiB + 512ULL * MiB))
        {
            // 6 GB RAM iPhones (e.g., iPhone 12 Pro / 13 Pro / 14 / 15): Jetsam limit ~2.85 GiB
            budget.tier = os::ios::DeviceMemoryTier::Mid6GB;
            budget.guestUserHeapBytes = 640ULL * MiB;
            budget.guestPhysicalHeapBytes = 640ULL * MiB;
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
            budget.guestUserHeapBytes = 1024ULL * MiB;
            budget.guestPhysicalHeapBytes = 1024ULL * MiB;
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
    EnsureSignalHandlersInstalled();
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

void os::ios::InstallGuestMemoryFaultHandler(uint8_t* guestBase, size_t guestSize, size_t pageSize)
{
    EnsureSignalHandlersInstalled();

    if (guestBase == nullptr || guestSize == 0)
        return;

    const size_t safePageSize = pageSize > 0 ? pageSize : 16384U;
    g_guestFaultPageSize.store(safePageSize, std::memory_order_release);
    g_guestFaultSize.store(guestSize, std::memory_order_release);
    g_guestFaultBase.store(reinterpret_cast<uintptr_t>(guestBase), std::memory_order_release);
}

void os::ios::ConfigureMetalLayer(void* metalLayer)
{
    if (metalLayer == nullptr)
        return;

    @autoreleasepool
    {
        CAMetalLayer* caLayer = (__bridge CAMetalLayer*)metalLayer;
        caLayer.maximumDrawableCount = 3;
        caLayer.allowsNextDrawableTimeout = YES;
    }
}

bool os::ios::IsMainThread()
{
    return pthread_main_np() != 0;
}

void os::ios::RunOnMainThreadSync(const std::function<void()>& fn)
{
    if (!fn)
        return;

    if (pthread_main_np() != 0)
    {
        fn();
        return;
    }

    dispatch_sync(dispatch_get_main_queue(), ^{
        fn();
    });
}

void os::ios::EnforceLandscapeOrientation()
{
    void (^apply)(void) = ^{
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        {
            if (![scene isKindOfClass:[UIWindowScene class]])
                continue;

            UIWindowScene* windowScene = (UIWindowScene*)scene;
            UIWindowSceneGeometryPreferencesIOS* prefs =
                [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
            [windowScene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError* error) {
                (void)error;
            }];
        }

        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        {
            if (![scene isKindOfClass:[UIWindowScene class]])
                continue;

            for (UIWindow* window in ((UIWindowScene*)scene).windows)
                [window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
        }
    };

    if (pthread_main_np() != 0)
        apply();
    else
        dispatch_async(dispatch_get_main_queue(), apply);
}

namespace
{
    enum TouchButtonIndex : int
    {
        kBtnDpadUp = 0,
        kBtnDpadDown,
        kBtnDpadLeft,
        kBtnDpadRight,
        kBtnA,
        kBtnB,
        kBtnX,
        kBtnY,
        kBtnLB,
        kBtnRB,
        kBtnLT,
        kBtnRT,
        kBtnBack,
        kBtnStart,
        kBtnCount
    };

    struct TouchPadButtonSpec
    {
        const char* label;
        SDL_GamepadButton sdlButton; // SDL_GAMEPAD_BUTTON_INVALID for analog-trigger buttons (LT/RT)
        uint16_t xamMask;
        bool isTrigger;
    };

    constexpr TouchPadButtonSpec kTouchPadButtons[kBtnCount] =
    {
        { "▲",     SDL_GAMEPAD_BUTTON_DPAD_UP,        XAMINPUT_GAMEPAD_DPAD_UP,        false },
        { "▼",     SDL_GAMEPAD_BUTTON_DPAD_DOWN,      XAMINPUT_GAMEPAD_DPAD_DOWN,      false },
        { "◀",     SDL_GAMEPAD_BUTTON_DPAD_LEFT,      XAMINPUT_GAMEPAD_DPAD_LEFT,      false },
        { "▶",     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     XAMINPUT_GAMEPAD_DPAD_RIGHT,     false },
        { "A",     SDL_GAMEPAD_BUTTON_SOUTH,          XAMINPUT_GAMEPAD_A,              false },
        { "B",     SDL_GAMEPAD_BUTTON_EAST,           XAMINPUT_GAMEPAD_B,              false },
        { "X",     SDL_GAMEPAD_BUTTON_WEST,           XAMINPUT_GAMEPAD_X,              false },
        { "Y",     SDL_GAMEPAD_BUTTON_NORTH,          XAMINPUT_GAMEPAD_Y,              false },
        { "LB",    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  XAMINPUT_GAMEPAD_LEFT_SHOULDER,  false },
        { "RB",    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER, false },
        { "LT",    SDL_GAMEPAD_BUTTON_INVALID,        0,                               true  },
        { "RT",    SDL_GAMEPAD_BUTTON_INVALID,        0,                               true  },
        { "BACK",  SDL_GAMEPAD_BUTTON_BACK,           XAMINPUT_GAMEPAD_BACK,           false },
        { "START", SDL_GAMEPAD_BUTTON_START,          XAMINPUT_GAMEPAD_START,          false }
    };

    SDL_JoystickID g_virtualJoystickId = 0;
    SDL_Joystick* g_virtualJoystick = nullptr;

    void EnsureVirtualGamepadAttached()
    {
        if (g_virtualJoystick != nullptr)
            return;

        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask =
            (1u << SDL_GAMEPAD_AXIS_LEFTX) |
            (1u << SDL_GAMEPAD_AXIS_LEFTY) |
            (1u << SDL_GAMEPAD_AXIS_RIGHTX) |
            (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
            (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) |
            (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        desc.button_mask =
            (1u << SDL_GAMEPAD_BUTTON_SOUTH) |
            (1u << SDL_GAMEPAD_BUTTON_EAST) |
            (1u << SDL_GAMEPAD_BUTTON_WEST) |
            (1u << SDL_GAMEPAD_BUTTON_NORTH) |
            (1u << SDL_GAMEPAD_BUTTON_BACK) |
            (1u << SDL_GAMEPAD_BUTTON_START) |
            (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
            (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        desc.name = "iReCompiled06 Xbox 360 Touch Controller";

        g_virtualJoystickId = SDL_AttachVirtualJoystick(&desc);
        if (g_virtualJoystickId != 0)
            g_virtualJoystick = SDL_OpenJoystick(g_virtualJoystickId);
    }

    void EmitTouchGamepadButtonTransition(SDL_GamepadButton sdlBtn, bool down)
    {
        if (sdlBtn == SDL_GAMEPAD_BUTTON_INVALID)
            return;

        EnsureVirtualGamepadAttached();

        if (g_virtualJoystick != nullptr)
        {
            SDL_SetJoystickVirtualButton(g_virtualJoystick, static_cast<int>(sdlBtn), down);
        }
        else
        {
            SDL_Event ev{};
            ev.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            ev.gbutton.timestamp = SDL_GetTicksNS();
            ev.gbutton.which = g_virtualJoystickId != 0 ? g_virtualJoystickId : 1;
            ev.gbutton.button = static_cast<Uint8>(sdlBtn);
            ev.gbutton.down = down;
            SDL_PushEvent(&ev);
        }
    }

    void EmitTouchGamepadAxis(SDL_GamepadAxis axis, Sint16 value)
    {
        EnsureVirtualGamepadAttached();

        if (g_virtualJoystick != nullptr)
        {
            SDL_SetJoystickVirtualAxis(g_virtualJoystick, static_cast<int>(axis), value);
        }
        else
        {
            SDL_Event ev{};
            ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
            ev.gaxis.timestamp = SDL_GetTicksNS();
            ev.gaxis.which = g_virtualJoystickId != 0 ? g_virtualJoystickId : 1;
            ev.gaxis.axis = static_cast<Uint8>(axis);
            ev.gaxis.value = value;
            SDL_PushEvent(&ev);
        }
    }

    int16_t ClampFloatToInt16(CGFloat normalized)
    {
        const CGFloat clamped = std::clamp<CGFloat>(normalized, -1.0, 1.0);
        const int scaled = static_cast<int>(std::lround(clamped * 32767.0));
        return static_cast<int16_t>(std::clamp(scaled, -32767, 32767));
    }
}

@interface MarathonTouchOverlayView : UIView
@end

@implementation MarathonTouchOverlayView
{
    UIView* _controlsContainer;
    UIView* _buttonViews[kBtnCount];
    UILabel* _buttonLabels[kBtnCount];
    bool _buttonDown[kBtnCount];

    UIView* _leftStickBase;
    UIView* _leftStickKnob;
    CGPoint _leftStickRestCenter;
    CGFloat _leftStickRadius;
    UITouch* _leftStickTouch;
    CGPoint _leftStickOrigin;

    UITouch* _rightLookTouch;
    CGPoint _rightLookOrigin;

    UIView* _toggleButtonView;
    UILabel* _toggleButtonLabel;
    UITouch* _toggleTouch;
    BOOL _controlsHidden;

    NSMutableSet<UITouch*>* _buttonTouches;

    int16_t _lastSdlLX;
    int16_t _lastSdlLY;
    int16_t _lastSdlRX;
    int16_t _lastSdlRY;
    int16_t _lastSdlLT;
    int16_t _lastSdlRT;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
    {
        self.backgroundColor = [UIColor clearColor];
        self.opaque = NO;
        self.multipleTouchEnabled = YES;
        self.userInteractionEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

        // IMPORTANT (MRC / Non-ARC): Use alloc+init so the set has +1 retain count and is never autoreleased
        // at the end of the installation @autoreleasepool (which previously caused recycled-address crashes in addObject:).
        _buttonTouches = [[NSMutableSet alloc] init];
        _controlsHidden = NO;
        _leftStickTouch = nil;
        _rightLookTouch = nil;
        _toggleTouch = nil;
        _leftStickRadius = 52.0;

        _controlsContainer = [[UIView alloc] initWithFrame:self.bounds];
        _controlsContainer.backgroundColor = [UIColor clearColor];
        _controlsContainer.opaque = NO;
        _controlsContainer.userInteractionEnabled = NO;
        _controlsContainer.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [self addSubview:_controlsContainer];
#if !__has_feature(objc_arc)
        [_controlsContainer release];
#endif

        // Virtual Left Analog Stick (base ring + movable thumb knob)
        _leftStickBase = [[UIView alloc] initWithFrame:CGRectZero];
        _leftStickBase.userInteractionEnabled = NO;
        _leftStickBase.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.28];
        _leftStickBase.layer.borderWidth = 1.5;
        _leftStickBase.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.38].CGColor;
        [_controlsContainer addSubview:_leftStickBase];
#if !__has_feature(objc_arc)
        [_leftStickBase release];
#endif

        _leftStickKnob = [[UIView alloc] initWithFrame:CGRectZero];
        _leftStickKnob.userInteractionEnabled = NO;
        _leftStickKnob.backgroundColor = [UIColor colorWithWhite:0.92 alpha:0.42];
        _leftStickKnob.layer.borderWidth = 1.5;
        _leftStickKnob.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.65].CGColor;
        [_controlsContainer addSubview:_leftStickKnob];
#if !__has_feature(objc_arc)
        [_leftStickKnob release];
#endif

        // Discrete Xbox 360 buttons (D-Pad, ABXY, LB/RB, LT/RT, BACK/START)
        for (int i = 0; i < kBtnCount; ++i)
        {
            _buttonDown[i] = false;

            UIView* btn = [[UIView alloc] initWithFrame:CGRectZero];
            btn.userInteractionEnabled = NO;
            btn.backgroundColor = [UIColor colorWithWhite:0.10 alpha:0.34];
            btn.layer.borderWidth = 1.5;

            if (i == kBtnA) // Xbox Green
                btn.layer.borderColor = [UIColor colorWithRed:0.25 green:0.95 blue:0.42 alpha:0.72].CGColor;
            else if (i == kBtnB) // Xbox Red
                btn.layer.borderColor = [UIColor colorWithRed:0.98 green:0.30 blue:0.30 alpha:0.72].CGColor;
            else if (i == kBtnX) // Xbox Blue
                btn.layer.borderColor = [UIColor colorWithRed:0.28 green:0.62 blue:1.00 alpha:0.72].CGColor;
            else if (i == kBtnY) // Xbox Yellow
                btn.layer.borderColor = [UIColor colorWithRed:1.00 green:0.84 blue:0.22 alpha:0.72].CGColor;
            else if (i == kBtnStart || i == kBtnBack)
                btn.layer.borderColor = [UIColor colorWithRed:0.75 green:0.85 blue:0.95 alpha:0.55].CGColor;
            else
                btn.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.44].CGColor;

            CGFloat fontSize = 16.0;
            if (i >= kBtnA && i <= kBtnY)
                fontSize = 18.0;
            else if (i == kBtnBack || i == kBtnStart)
                fontSize = 11.0;
            else if (i >= kBtnLB && i <= kBtnRT)
                fontSize = 13.0;
            else
                fontSize = 14.0;

            UILabel* lbl = [[UILabel alloc] initWithFrame:CGRectZero];
            lbl.text = [NSString stringWithUTF8String:kTouchPadButtons[i].label];
            lbl.textAlignment = NSTextAlignmentCenter;
            lbl.textColor = [UIColor colorWithWhite:1.0 alpha:0.86];
            lbl.font = [UIFont boldSystemFontOfSize:fontSize];
            [btn addSubview:lbl];

            [_controlsContainer addSubview:btn];
            _buttonViews[i] = btn;
            _buttonLabels[i] = lbl;

#if !__has_feature(objc_arc)
            [lbl release];
            [btn release];
#endif
        }

        // Bottom-left hide/show toggle button (">>" / "<<")
        _toggleButtonView = [[UIView alloc] initWithFrame:CGRectZero];
        _toggleButtonView.userInteractionEnabled = NO;
        _toggleButtonView.backgroundColor = [UIColor colorWithWhite:0.10 alpha:0.38];
        _toggleButtonView.layer.borderWidth = 1.2;
        _toggleButtonView.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.45].CGColor;

        _toggleButtonLabel = [[UILabel alloc] initWithFrame:CGRectZero];
        _toggleButtonLabel.text = @">>";
        _toggleButtonLabel.textAlignment = NSTextAlignmentCenter;
        _toggleButtonLabel.textColor = [UIColor colorWithWhite:1.0 alpha:0.85];
        _toggleButtonLabel.font = [UIFont boldSystemFontOfSize:12.0];
        [_toggleButtonView addSubview:_toggleButtonLabel];
        [self addSubview:_toggleButtonView];

#if !__has_feature(objc_arc)
        [_toggleButtonLabel release];
        [_toggleButtonView release];
#endif
    }
    return self;
}

#if !__has_feature(objc_arc)
- (void)dealloc
{
    [_buttonTouches release];
    _buttonTouches = nil;
    [super dealloc];
}
#endif

- (void)layoutSubviews
{
    [super layoutSubviews];

    _controlsContainer.frame = self.bounds;

    const CGRect bounds = self.bounds;
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat leftPad = MAX(safe.left, 16.0) + 10.0;
    const CGFloat rightPad = MAX(safe.right, 16.0) + 12.0;
    const CGFloat topPad = MAX(safe.top, 6.0) + 4.0;
    const CGFloat bottomPad = MAX(safe.bottom, 10.0) + 8.0;

    // 1. Hide/Show toggle button (bottom-left corner ">>")
    const CGFloat toggleW = 36.0;
    const CGFloat toggleH = 26.0;
    _toggleButtonView.frame = CGRectMake(MAX(safe.left, 8.0) + 4.0, bounds.size.height - MAX(safe.bottom, 6.0) - toggleH - 4.0, toggleW, toggleH);
    _toggleButtonView.layer.cornerRadius = toggleH * 0.5;
    _toggleButtonLabel.frame = _toggleButtonView.bounds;

    // 2. Bottom-left Left Analog Stick
    _leftStickRadius = 48.0;
    const CGFloat stickDiameter = _leftStickRadius * 2.0;
    const CGFloat knobDiameter = 46.0;
    const CGFloat stickCenterX = leftPad + _leftStickRadius + 20.0;
    const CGFloat stickCenterY = bounds.size.height - bottomPad - _leftStickRadius - 10.0;
    _leftStickRestCenter = CGPointMake(stickCenterX, stickCenterY);

    _leftStickBase.frame = CGRectMake(stickCenterX - _leftStickRadius, stickCenterY - _leftStickRadius, stickDiameter, stickDiameter);
    _leftStickBase.layer.cornerRadius = _leftStickRadius;

    if (_leftStickTouch == nil)
    {
        _leftStickKnob.frame = CGRectMake(stickCenterX - knobDiameter * 0.5, stickCenterY - knobDiameter * 0.5, knobDiameter, knobDiameter);
    }
    _leftStickKnob.layer.cornerRadius = knobDiameter * 0.5;

    // 3. Compact D-Pad shifted right & down so its top button never overlaps the middle-left Sonic lives HUD
    const CGFloat dpadSize = 30.0;
    const CGFloat dpadStep = 29.0;
    const CGFloat dpadHalfSpan = dpadStep + dpadSize * 0.5;
    const CGFloat dpadCenterX = leftPad + dpadHalfSpan + 48.0;
    const CGFloat dpadCenterY = MAX(bounds.size.height * 0.415 + dpadHalfSpan,
                                    stickCenterY - _leftStickRadius - dpadHalfSpan - 8.0);

    const CGPoint dpadCenters[4] =
    {
        CGPointMake(dpadCenterX, dpadCenterY - dpadStep), // Up
        CGPointMake(dpadCenterX, dpadCenterY + dpadStep), // Down
        CGPointMake(dpadCenterX - dpadStep, dpadCenterY), // Left
        CGPointMake(dpadCenterX + dpadStep, dpadCenterY)  // Right
    };

    for (int i = 0; i < 4; ++i)
    {
        _buttonViews[i].frame = CGRectMake(dpadCenters[i].x - dpadSize * 0.5, dpadCenters[i].y - dpadSize * 0.5, dpadSize, dpadSize);
        _buttonViews[i].layer.cornerRadius = 8.0;
        _buttonLabels[i].frame = _buttonViews[i].bounds;
    }

    // 4. Right-side Xbox 360 ABXY diamond shifted upward between the top-right Minimap and bottom-right Action Gauge
    const CGFloat abxySize = 46.0;
    const CGFloat diamondRadius = 43.0;
    const CGFloat diamondHalfSpan = diamondRadius + abxySize * 0.5;
    const CGFloat diamondCenterX = bounds.size.width - rightPad - diamondHalfSpan - 8.0;
    const CGFloat gaugeSafeBottomY = MIN(bounds.size.height - bottomPad - 56.0, bounds.size.height * 0.780);
    const CGFloat diamondCenterY = MAX(bounds.size.height * 0.445 + diamondHalfSpan,
                                       gaugeSafeBottomY - diamondHalfSpan);

    const CGPoint abxyCenters[4] =
    {
        CGPointMake(diamondCenterX, diamondCenterY + diamondRadius), // A (South / bottom)
        CGPointMake(diamondCenterX + diamondRadius, diamondCenterY), // B (East / right)
        CGPointMake(diamondCenterX - diamondRadius, diamondCenterY), // X (West / left)
        CGPointMake(diamondCenterX, diamondCenterY - diamondRadius)  // Y (North / top)
    };

    for (int idx = 0; idx < 4; ++idx)
    {
        const int btnIdx = kBtnA + idx;
        _buttonViews[btnIdx].frame = CGRectMake(abxyCenters[idx].x - abxySize * 0.5, abxyCenters[idx].y - abxySize * 0.5, abxySize, abxySize);
        _buttonViews[btnIdx].layer.cornerRadius = abxySize * 0.5;
        _buttonLabels[btnIdx].frame = _buttonViews[btnIdx].bounds;
    }

    // 5. Top-left LT / LB and Top-right RT / RB shoulder & trigger buttons
    const CGFloat shoulderW = 62.0;
    const CGFloat shoulderH = 30.0;
    const CGFloat shoulderGap = 10.0;

    // Left: LT outermost top, LB right next to it
    _buttonViews[kBtnLT].frame = CGRectMake(leftPad, topPad, shoulderW, shoulderH);
    _buttonViews[kBtnLT].layer.cornerRadius = 9.0;
    _buttonLabels[kBtnLT].frame = _buttonViews[kBtnLT].bounds;

    _buttonViews[kBtnLB].frame = CGRectMake(leftPad + shoulderW + shoulderGap, topPad, shoulderW, shoulderH);
    _buttonViews[kBtnLB].layer.cornerRadius = 9.0;
    _buttonLabels[kBtnLB].frame = _buttonViews[kBtnLB].bounds;

    // Right: RB + RT mirrored at top-right
    const CGFloat rtX = bounds.size.width - rightPad - shoulderW;
    const CGFloat rbX = rtX - shoulderGap - shoulderW;

    _buttonViews[kBtnRB].frame = CGRectMake(rbX, topPad, shoulderW, shoulderH);
    _buttonViews[kBtnRB].layer.cornerRadius = 9.0;
    _buttonLabels[kBtnRB].frame = _buttonViews[kBtnRB].bounds;

    _buttonViews[kBtnRT].frame = CGRectMake(rtX, topPad, shoulderW, shoulderH);
    _buttonViews[kBtnRT].layer.cornerRadius = 9.0;
    _buttonLabels[kBtnRT].frame = _buttonViews[kBtnRT].bounds;

    // 6. Top-center compact BACK and START buttons
    const CGFloat metaW = 58.0;
    const CGFloat metaH = 26.0;
    const CGFloat metaGap = 16.0;
    const CGFloat midX = bounds.size.width * 0.5;

    _buttonViews[kBtnBack].frame = CGRectMake(midX - metaGap * 0.5 - metaW, topPad + 2.0, metaW, metaH);
    _buttonViews[kBtnBack].layer.cornerRadius = metaH * 0.5;
    _buttonLabels[kBtnBack].frame = _buttonViews[kBtnBack].bounds;

    _buttonViews[kBtnStart].frame = CGRectMake(midX + metaGap * 0.5, topPad + 2.0, metaW, metaH);
    _buttonViews[kBtnStart].layer.cornerRadius = metaH * 0.5;
    _buttonLabels[kBtnStart].frame = _buttonViews[kBtnStart].bounds;
}

- (BOOL)isPointInsideToggle:(CGPoint)point
{
    return CGRectContainsPoint(CGRectInset(_toggleButtonView.frame, -10.0, -10.0), point);
}

- (int)hitButtonAtPoint:(CGPoint)point
{
    for (int i = 0; i < kBtnCount; ++i)
    {
        if (CGRectContainsPoint(CGRectInset(_buttonViews[i].frame, -8.0, -8.0), point))
            return i;
    }
    return -1;
}

- (BOOL)isPointInsideLeftStickZone:(CGPoint)point
{
    const CGFloat dx = point.x - _leftStickRestCenter.x;
    const CGFloat dy = point.y - _leftStickRestCenter.y;
    const CGFloat hitR = _leftStickRadius + 24.0;
    return (dx * dx + dy * dy) <= (hitR * hitR);
}

- (BOOL)isPointInsideRightCameraZone:(CGPoint)point
{
    // Right half of the screen outside discrete buttons acts as virtual Right Stick camera drag pad.
    return point.x >= (self.bounds.size.width * 0.48);
}

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event
{
    if ([self isPointInsideToggle:point])
        return YES;

    if (_controlsHidden)
        return NO;

    if ([self hitButtonAtPoint:point] >= 0)
        return YES;

    if ([self isPointInsideLeftStickZone:point])
        return YES;

    if ([self isPointInsideRightCameraZone:point])
        return YES;

    return NO;
}

- (void)resetAllInputsAndVisuals
{
    [_buttonTouches removeAllObjects];
    _leftStickTouch = nil;
    _rightLookTouch = nil;

    const CGFloat knobDiameter = 46.0;
    _leftStickKnob.frame = CGRectMake(
        _leftStickRestCenter.x - knobDiameter * 0.5,
        _leftStickRestCenter.y - knobDiameter * 0.5,
        knobDiameter,
        knobDiameter);

    for (int i = 0; i < kBtnCount; ++i)
    {
        if (_buttonDown[i])
        {
            _buttonDown[i] = false;
            _buttonViews[i].backgroundColor = [UIColor colorWithWhite:0.10 alpha:0.34];
            EmitTouchGamepadButtonTransition(kTouchPadButtons[i].sdlButton, false);
        }
    }

    if (_lastSdlLX != 0) { _lastSdlLX = 0; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, 0); }
    if (_lastSdlLY != 0) { _lastSdlLY = 0; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, 0); }
    if (_lastSdlRX != 0) { _lastSdlRX = 0; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_RIGHTX, 0); }
    if (_lastSdlRY != 0) { _lastSdlRY = 0; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_RIGHTY, 0); }
    if (_lastSdlLT != 0) { _lastSdlLT = 0; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0); }
    if (_lastSdlRT != 0) { _lastSdlRT = 0; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0); }

    HID::SetVirtualGamepadState(0, 0, 0, 0, 0, 0, 0);
}

- (void)recomputeControllerState
{
    if (_controlsHidden)
        return;

    uint16_t xamButtons = 0;
    bool nextButtonDown[kBtnCount]{};

    // 1. Evaluate discrete button touches
    for (UITouch* touch in _buttonTouches)
    {
        if (touch.phase == UITouchPhaseEnded || touch.phase == UITouchPhaseCancelled)
            continue;

        const CGPoint pt = [touch locationInView:self];
        for (int i = 0; i < kBtnCount; ++i)
        {
            if (CGRectContainsPoint(CGRectInset(_buttonViews[i].frame, -9.0, -9.0), pt))
            {
                nextButtonDown[i] = true;
                xamButtons |= kTouchPadButtons[i].xamMask;
            }
        }
    }

    for (int i = 0; i < kBtnCount; ++i)
    {
        if (_buttonDown[i] != nextButtonDown[i])
        {
            _buttonDown[i] = nextButtonDown[i];
            _buttonViews[i].backgroundColor = nextButtonDown[i]
                ? [UIColor colorWithWhite:0.86 alpha:0.56]
                : [UIColor colorWithWhite:0.10 alpha:0.34];
            EmitTouchGamepadButtonTransition(kTouchPadButtons[i].sdlButton, nextButtonDown[i]);
        }
    }

    const uint8_t xamLT = nextButtonDown[kBtnLT] ? 255u : 0u;
    const uint8_t xamRT = nextButtonDown[kBtnRT] ? 255u : 0u;
    const int16_t sdlLT = nextButtonDown[kBtnLT] ? 32767 : 0;
    const int16_t sdlRT = nextButtonDown[kBtnRT] ? 32767 : 0;
    if (sdlLT != _lastSdlLT) { _lastSdlLT = sdlLT; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, sdlLT); }
    if (sdlRT != _lastSdlRT) { _lastSdlRT = sdlRT; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, sdlRT); }

    // 2. Evaluate Left Analog Stick (dynamic origin anchored where finger landed, clamped to _leftStickRadius)
    int16_t sdlLX = 0;
    int16_t sdlLY = 0;
    int16_t xamLX = 0;
    int16_t xamLY = 0;
    const CGFloat knobDiameter = 46.0;

    if (_leftStickTouch != nil &&
        _leftStickTouch.phase != UITouchPhaseEnded &&
        _leftStickTouch.phase != UITouchPhaseCancelled)
    {
        const CGPoint pt = [_leftStickTouch locationInView:self];
        CGFloat dx = pt.x - _leftStickOrigin.x;
        CGFloat dy = pt.y - _leftStickOrigin.y;
        const CGFloat dist = std::hypot(dx, dy);
        if (dist > _leftStickRadius && dist > 0.0001)
        {
            const CGFloat inv = _leftStickRadius / dist;
            dx *= inv;
            dy *= inv;
        }

        const CGPoint knobCenter = CGPointMake(_leftStickRestCenter.x + dx, _leftStickRestCenter.y + dy);
        _leftStickKnob.frame = CGRectMake(knobCenter.x - knobDiameter * 0.5, knobCenter.y - knobDiameter * 0.5, knobDiameter, knobDiameter);

        const CGFloat normX = dx / _leftStickRadius;
        const CGFloat normY = dy / _leftStickRadius; // +down in UIKit / SDL, -down in XInput
        const CGFloat mag = std::hypot(normX, normY);
        if (mag > 0.12)
        {
            sdlLX = ClampFloatToInt16(normX);
            sdlLY = ClampFloatToInt16(normY);
            xamLX = ClampFloatToInt16(normX);
            xamLY = ClampFloatToInt16(-normY);
        }
    }
    else
    {
        _leftStickTouch = nil;
        _leftStickKnob.frame = CGRectMake(
            _leftStickRestCenter.x - knobDiameter * 0.5,
            _leftStickRestCenter.y - knobDiameter * 0.5,
            knobDiameter,
            knobDiameter);
    }

    if (sdlLX != _lastSdlLX) { _lastSdlLX = sdlLX; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, sdlLX); }
    if (sdlLY != _lastSdlLY) { _lastSdlLY = sdlLY; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, sdlLY); }

    // 3. Evaluate Right Analog Camera Drag Area (virtual stick centered on touch-down point)
    int16_t sdlRX = 0;
    int16_t sdlRY = 0;
    int16_t xamRX = 0;
    int16_t xamRY = 0;
    constexpr CGFloat kRightCameraRadius = 68.0;

    if (_rightLookTouch != nil &&
        _rightLookTouch.phase != UITouchPhaseEnded &&
        _rightLookTouch.phase != UITouchPhaseCancelled)
    {
        const CGPoint pt = [_rightLookTouch locationInView:self];
        const CGFloat dx = pt.x - _rightLookOrigin.x;
        const CGFloat dy = pt.y - _rightLookOrigin.y;
        const CGFloat normX = std::clamp<CGFloat>(dx / kRightCameraRadius, -1.0, 1.0);
        const CGFloat normY = std::clamp<CGFloat>(dy / kRightCameraRadius, -1.0, 1.0);
        if (std::hypot(normX, normY) > 0.08)
        {
            sdlRX = ClampFloatToInt16(normX);
            sdlRY = ClampFloatToInt16(normY);
            xamRX = ClampFloatToInt16(normX);
            xamRY = ClampFloatToInt16(-normY);
        }
    }
    else
    {
        _rightLookTouch = nil;
    }

    if (sdlRX != _lastSdlRX) { _lastSdlRX = sdlRX; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_RIGHTX, sdlRX); }
    if (sdlRY != _lastSdlRY) { _lastSdlRY = sdlRY; EmitTouchGamepadAxis(SDL_GAMEPAD_AXIS_RIGHTY, sdlRY); }

    HID::SetVirtualGamepadState(xamButtons, xamLT, xamRT, xamLX, xamLY, xamRX, xamRY);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        const CGPoint pt = [t locationInView:self];

        if ([self isPointInsideToggle:pt])
        {
            _toggleTouch = t;
            _controlsHidden = !_controlsHidden;
            _controlsContainer.hidden = _controlsHidden;
            _toggleButtonLabel.text = _controlsHidden ? @"<<" : @">>";
            if (_controlsHidden)
                [self resetAllInputsAndVisuals];
            continue;
        }

        if (_controlsHidden)
            continue;

        if ([self hitButtonAtPoint:pt] >= 0)
        {
            if (_buttonTouches == nil)
                _buttonTouches = [[NSMutableSet alloc] init];
            [_buttonTouches addObject:t];
        }
        else if (_leftStickTouch == nil && [self isPointInsideLeftStickZone:pt])
        {
            _leftStickTouch = t;
            _leftStickOrigin = pt;
        }
        else if (_rightLookTouch == nil && [self isPointInsideRightCameraZone:pt])
        {
            _rightLookTouch = t;
            _rightLookOrigin = pt;
        }
    }

    [self recomputeControllerState];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self recomputeControllerState];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        if (t == _toggleTouch)
            _toggleTouch = nil;
        if (t == _leftStickTouch)
            _leftStickTouch = nil;
        if (t == _rightLookTouch)
            _rightLookTouch = nil;
        if (_buttonTouches != nil)
            [_buttonTouches removeObject:t];
    }

    [self recomputeControllerState];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        if (t == _toggleTouch)
            _toggleTouch = nil;
        if (t == _leftStickTouch)
            _leftStickTouch = nil;
        if (t == _rightLookTouch)
            _rightLookTouch = nil;
        if (_buttonTouches != nil)
            [_buttonTouches removeObject:t];
    }

    [self recomputeControllerState];
}

@end

static MarathonTouchOverlayView* g_touchOverlayInstance = nil;

void os::ios::InstallTouchControllerOverlay(void* uiWindowPtr)
{
    if (uiWindowPtr == nullptr)
        return;

    os::ios::RunOnMainThreadSync([uiWindowPtr]()
    {
        @autoreleasepool
        {
            EnsureVirtualGamepadAttached();

            UIWindow* window = (__bridge UIWindow*)uiWindowPtr;
            UIView* rootView = window.rootViewController != nil ? window.rootViewController.view : window;
            if (rootView == nil)
                return;

            if (g_touchOverlayInstance != nil)
            {
                if (g_touchOverlayInstance.superview != rootView)
                {
                    g_touchOverlayInstance.frame = rootView.bounds;
                    [rootView addSubview:g_touchOverlayInstance];
                }
                [rootView bringSubviewToFront:g_touchOverlayInstance];
                return;
            }

            MarathonTouchOverlayView* overlay = [[MarathonTouchOverlayView alloc] initWithFrame:rootView.bounds];
            g_touchOverlayInstance = overlay; // Retained globally (+1 in Non-ARC, strong in ARC)
            [rootView addSubview:overlay];
            [rootView bringSubviewToFront:overlay];
        }
    });
}
