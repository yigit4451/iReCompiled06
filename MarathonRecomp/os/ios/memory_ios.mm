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
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <mutex>
#include <os/proc.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
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
        if (info != nullptr)
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

        ForwardUnhandledSignal(sig, info, ucontext, sig == SIGBUS ? g_prevSigbusAction : g_prevSigsegvAction);
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
    if (guestBase == nullptr || guestSize == 0)
        return;

    const size_t safePageSize = pageSize > 0 ? pageSize : 16384U;
    g_guestFaultPageSize.store(safePageSize, std::memory_order_release);
    g_guestFaultSize.store(guestSize, std::memory_order_release);
    g_guestFaultBase.store(reinterpret_cast<uintptr_t>(guestBase), std::memory_order_release);

    std::call_once(g_faultHandlerOnceFlag, []()
    {
        struct sigaction act{};
        act.sa_sigaction = GuestMemorySignalHandler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_SIGINFO | SA_NODEFER;

        sigaction(SIGBUS, &act, &g_prevSigbusAction);
        sigaction(SIGSEGV, &act, &g_prevSigsegvAction);
    });
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
    struct TouchPadButtonSpec
    {
        const char* label;
        SDL_GamepadButton sdlButton;
        uint16_t xamMask;
    };

    constexpr TouchPadButtonSpec kTouchPadButtons[7] =
    {
        { "▲",     SDL_GAMEPAD_BUTTON_DPAD_UP,    XAMINPUT_GAMEPAD_DPAD_UP },
        { "▼",     SDL_GAMEPAD_BUTTON_DPAD_DOWN,  XAMINPUT_GAMEPAD_DPAD_DOWN },
        { "◀",     SDL_GAMEPAD_BUTTON_DPAD_LEFT,  XAMINPUT_GAMEPAD_DPAD_LEFT },
        { "▶",     SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
        { "START", SDL_GAMEPAD_BUTTON_START,      XAMINPUT_GAMEPAD_START },
        { "A",     SDL_GAMEPAD_BUTTON_SOUTH,      XAMINPUT_GAMEPAD_A },
        { "B",     SDL_GAMEPAD_BUTTON_EAST,       XAMINPUT_GAMEPAD_B }
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
        desc.button_mask =
            (1u << SDL_GAMEPAD_BUTTON_SOUTH) |
            (1u << SDL_GAMEPAD_BUTTON_EAST) |
            (1u << SDL_GAMEPAD_BUTTON_WEST) |
            (1u << SDL_GAMEPAD_BUTTON_NORTH) |
            (1u << SDL_GAMEPAD_BUTTON_START) |
            (1u << SDL_GAMEPAD_BUTTON_BACK) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
            (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        desc.name = "iReCompiled06 Touch Gamepad";

        g_virtualJoystickId = SDL_AttachVirtualJoystick(&desc);
        if (g_virtualJoystickId != 0)
            g_virtualJoystick = SDL_OpenJoystick(g_virtualJoystickId);
    }

    void EmitTouchGamepadButtonTransition(SDL_GamepadButton sdlBtn, bool down)
    {
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
}

@interface MarathonTouchOverlayView : UIView
@end

@implementation MarathonTouchOverlayView
{
    UIView* _buttonViews[7];
    UILabel* _buttonLabels[7];
    uint16_t _pressedMask;
    NSMutableSet<UITouch*>* _activeTouches;
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
        _pressedMask = 0;
        _activeTouches = [NSMutableSet set];

        for (int i = 0; i < 7; ++i)
        {
            UIView* btn = [[UIView alloc] initWithFrame:CGRectZero];
            btn.userInteractionEnabled = NO;
            btn.backgroundColor = [UIColor colorWithWhite:0.10 alpha:0.36];
            btn.layer.borderWidth = 1.5;

            if (i == 5) // A (green)
                btn.layer.borderColor = [UIColor colorWithRed:0.25 green:0.95 blue:0.45 alpha:0.65].CGColor;
            else if (i == 6) // B (red)
                btn.layer.borderColor = [UIColor colorWithRed:0.98 green:0.32 blue:0.32 alpha:0.65].CGColor;
            else if (i == 4) // START (cyan)
                btn.layer.borderColor = [UIColor colorWithRed:0.35 green:0.85 blue:1.00 alpha:0.65].CGColor;
            else
                btn.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.45].CGColor;

            UILabel* lbl = [[UILabel alloc] initWithFrame:CGRectZero];
            lbl.text = [NSString stringWithUTF8String:kTouchPadButtons[i].label];
            lbl.textAlignment = NSTextAlignmentCenter;
            lbl.textColor = [UIColor colorWithWhite:1.0 alpha:0.85];
            lbl.font = [UIFont boldSystemFontOfSize:(i == 4 ? 13.0 : 19.0)];
            [btn addSubview:lbl];

            [self addSubview:btn];
            _buttonViews[i] = btn;
            _buttonLabels[i] = lbl;
        }
    }
    return self;
}

- (void)layoutSubviews
{
    [super layoutSubviews];

    const CGRect bounds = self.bounds;
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat leftPad = MAX(safe.left, 18.0) + 12.0;
    const CGFloat rightPad = MAX(safe.right, 18.0) + 16.0;
    const CGFloat bottomPad = MAX(safe.bottom, 14.0) + 14.0;

    // Bottom-left D-Pad cross
    const CGFloat dpadSize = 48.0;
    const CGFloat dpadStep = 50.0;
    const CGFloat dpadCenterX = leftPad + dpadStep + dpadSize * 0.5;
    const CGFloat dpadCenterY = bounds.size.height - bottomPad - dpadStep - dpadSize * 0.5;

    const CGPoint dpadCenters[4] =
    {
        CGPointMake(dpadCenterX, dpadCenterY - dpadStep), // Up
        CGPointMake(dpadCenterX, dpadCenterY + dpadStep), // Down
        CGPointMake(dpadCenterX - dpadStep, dpadCenterY), // Left
        CGPointMake(dpadCenterX + dpadStep, dpadCenterY)  // Right
    };

    for (int i = 0; i < 4; ++i)
    {
        CGRect f = CGRectMake(dpadCenters[i].x - dpadSize * 0.5, dpadCenters[i].y - dpadSize * 0.5, dpadSize, dpadSize);
        _buttonViews[i].frame = f;
        _buttonViews[i].layer.cornerRadius = 12.0;
        _buttonLabels[i].frame = _buttonViews[i].bounds;
    }

    // Bottom-right Start, A, B buttons
    const CGFloat abSize = 58.0;
    const CGFloat bCenterX = bounds.size.width - rightPad - abSize * 0.5;
    const CGFloat bCenterY = bounds.size.height - bottomPad - abSize * 0.5 - 38.0;
    const CGFloat aCenterX = bCenterX - 66.0;
    const CGFloat aCenterY = bounds.size.height - bottomPad - abSize * 0.5 + 4.0;

    _buttonViews[5].frame = CGRectMake(aCenterX - abSize * 0.5, aCenterY - abSize * 0.5, abSize, abSize);
    _buttonViews[5].layer.cornerRadius = abSize * 0.5;
    _buttonLabels[5].frame = _buttonViews[5].bounds;

    _buttonViews[6].frame = CGRectMake(bCenterX - abSize * 0.5, bCenterY - abSize * 0.5, abSize, abSize);
    _buttonViews[6].layer.cornerRadius = abSize * 0.5;
    _buttonLabels[6].frame = _buttonViews[6].bounds;

    const CGFloat startW = 78.0;
    const CGFloat startH = 36.0;
    const CGFloat startX = aCenterX - 96.0 - startW * 0.5;
    const CGFloat startY = bounds.size.height - bottomPad - startH - 4.0;
    _buttonViews[4].frame = CGRectMake(startX, startY, startW, startH);
    _buttonViews[4].layer.cornerRadius = startH * 0.5;
    _buttonLabels[4].frame = _buttonViews[4].bounds;
}

// Pass touches outside button hit zones straight through to the underlying SDL view.
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event
{
    for (int i = 0; i < 7; ++i)
    {
        CGRect hitRect = CGRectInset(_buttonViews[i].frame, -8.0, -8.0);
        if (CGRectContainsPoint(hitRect, point))
            return YES;
    }
    return NO;
}

- (void)recomputeButtonStates
{
    uint16_t newMask = 0;
    bool nextPressed[7]{};

    for (UITouch* touch in _activeTouches)
    {
        if (touch.phase == UITouchPhaseEnded || touch.phase == UITouchPhaseCancelled)
            continue;

        const CGPoint pt = [touch locationInView:self];
        for (int i = 0; i < 7; ++i)
        {
            CGRect hitRect = CGRectInset(_buttonViews[i].frame, -10.0, -10.0);
            if (CGRectContainsPoint(hitRect, pt))
            {
                nextPressed[i] = true;
                newMask |= kTouchPadButtons[i].xamMask;
            }
        }
    }

    HID::SetVirtualGamepadButtons(newMask);

    for (int i = 0; i < 7; ++i)
    {
        const bool wasDown = (_pressedMask & kTouchPadButtons[i].xamMask) != 0;
        const bool isDown = nextPressed[i];
        if (wasDown != isDown)
        {
            _buttonViews[i].backgroundColor = isDown
                ? [UIColor colorWithWhite:0.85 alpha:0.55]
                : [UIColor colorWithWhite:0.10 alpha:0.36];
            EmitTouchGamepadButtonTransition(kTouchPadButtons[i].sdlButton, isDown);
        }
    }

    _pressedMask = newMask;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
        [_activeTouches addObject:t];
    [self recomputeButtonStates];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self recomputeButtonStates];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
        [_activeTouches removeObject:t];
    [self recomputeButtonStates];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
        [_activeTouches removeObject:t];
    [self recomputeButtonStates];
}

@end

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

            for (UIView* sub in rootView.subviews)
            {
                if ([sub isKindOfClass:[MarathonTouchOverlayView class]])
                {
                    [rootView bringSubviewToFront:sub];
                    return;
                }
            }

            MarathonTouchOverlayView* overlay = [[MarathonTouchOverlayView alloc] initWithFrame:rootView.bounds];
            [rootView addSubview:overlay];
            [rootView bringSubviewToFront:overlay];
        }
    });
}
