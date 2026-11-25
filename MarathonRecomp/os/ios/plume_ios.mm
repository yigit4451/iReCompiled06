// iOS UIKit replacement for thirdparty/plume/plume_apple.mm (which imports macOS-only AppKit & IOKit).
// Provides the exact plume::getRenderDeviceVendor and plume::CocoaWindow symbols backed by UIWindow/UIScreen.

#include "plume_apple.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <cmath>

namespace plume
{
    RenderDeviceVendor getRenderDeviceVendor(uint64_t /*registryID*/)
    {
        // All iOS / iPadOS devices run Apple Silicon GPUs.
        return RenderDeviceVendor::APPLE;
    }

    static CGFloat getScaleFactor(UIWindow* uiWindow)
    {
#ifdef PLUME_APPLE_RETINA_ENABLED
        if (uiWindow && uiWindow.screen)
            return uiWindow.screen.nativeScale;
        return [UIScreen mainScreen].nativeScale;
#else
        return 1.0f;
#endif
    }

    static UIScreen* getWindowScreen(UIWindow* uiWindow)
    {
        if (uiWindow && uiWindow.screen)
            return uiWindow.screen;
        return [UIScreen mainScreen];
    }

    CocoaWindow::CocoaWindow(void* window)
        : windowHandle(window), cachedRefreshRate(60)
    {
        cachedAttributes = { 0, 0, 0, 0 };

        if ([NSThread isMainThread])
        {
            UIWindow* uiWindow = (__bridge UIWindow*)windowHandle;
            UIScreen* screen = getWindowScreen(uiWindow);
            CGRect bounds = uiWindow ? uiWindow.bounds : screen.bounds;
            CGFloat scaleFactor = getScaleFactor(uiWindow);

            cachedAttributes.x = static_cast<int>(std::round(bounds.origin.x));
            cachedAttributes.y = static_cast<int>(std::round(bounds.origin.y));
            cachedAttributes.width = static_cast<int>(std::round(bounds.size.width * scaleFactor));
            cachedAttributes.height = static_cast<int>(std::round(bounds.size.height * scaleFactor));

            if (screen)
                cachedRefreshRate.store(static_cast<int>(screen.maximumFramesPerSecond));
        }
        else
        {
            updateWindowAttributesInternal(true);
            updateRefreshRateInternal(true);
        }
    }

    CocoaWindow::~CocoaWindow() = default;

    void CocoaWindow::updateWindowAttributesInternal(bool forceSync)
    {
        auto updateBlock = ^{
            UIWindow* uiWindow = (__bridge UIWindow*)windowHandle;
            UIScreen* screen = getWindowScreen(uiWindow);
            CGRect bounds = uiWindow ? uiWindow.bounds : screen.bounds;
            CGFloat scaleFactor = getScaleFactor(uiWindow);

            std::lock_guard<std::mutex> lock(attributesMutex);
            cachedAttributes.x = static_cast<int>(std::round(bounds.origin.x));
            cachedAttributes.y = static_cast<int>(std::round(bounds.origin.y));
            cachedAttributes.width = static_cast<int>(std::round(bounds.size.width * scaleFactor));
            cachedAttributes.height = static_cast<int>(std::round(bounds.size.height * scaleFactor));
        };

        if (forceSync)
            dispatch_sync(dispatch_get_main_queue(), updateBlock);
        else
            dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    void CocoaWindow::updateRefreshRateInternal(bool forceSync)
    {
        auto updateBlock = ^{
            UIWindow* uiWindow = (__bridge UIWindow*)windowHandle;
            UIScreen* screen = getWindowScreen(uiWindow);
            if (screen)
                cachedRefreshRate.store(static_cast<int>(screen.maximumFramesPerSecond));
        };

        if (forceSync)
            dispatch_sync(dispatch_get_main_queue(), updateBlock);
        else
            dispatch_async(dispatch_get_main_queue(), updateBlock);
    }

    void CocoaWindow::getWindowAttributes(CocoaWindowAttributes* attributes) const
    {
        if ([NSThread isMainThread])
        {
            UIWindow* uiWindow = (__bridge UIWindow*)windowHandle;
            UIScreen* screen = getWindowScreen(uiWindow);
            CGRect bounds = uiWindow ? uiWindow.bounds : screen.bounds;
            CGFloat scaleFactor = getScaleFactor(uiWindow);

            std::lock_guard<std::mutex> lock(attributesMutex);
            const_cast<CocoaWindow*>(this)->cachedAttributes.x = static_cast<int>(std::round(bounds.origin.x));
            const_cast<CocoaWindow*>(this)->cachedAttributes.y = static_cast<int>(std::round(bounds.origin.y));
            const_cast<CocoaWindow*>(this)->cachedAttributes.width = static_cast<int>(std::round(bounds.size.width * scaleFactor));
            const_cast<CocoaWindow*>(this)->cachedAttributes.height = static_cast<int>(std::round(bounds.size.height * scaleFactor));

            *attributes = cachedAttributes;
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(attributesMutex);
                *attributes = cachedAttributes;
            }
            const_cast<CocoaWindow*>(this)->updateWindowAttributesInternal(false);
        }
    }

    int CocoaWindow::getRefreshRate() const
    {
        if ([NSThread isMainThread])
        {
            UIWindow* uiWindow = (__bridge UIWindow*)windowHandle;
            UIScreen* screen = getWindowScreen(uiWindow);
            if (screen)
            {
                int rate = static_cast<int>(screen.maximumFramesPerSecond);
                const_cast<CocoaWindow*>(this)->cachedRefreshRate.store(rate);
                return rate;
            }
            return cachedRefreshRate.load();
        }
        else
        {
            int rate = cachedRefreshRate.load();
            const_cast<CocoaWindow*>(this)->updateRefreshRateInternal(false);
            return rate;
        }
    }

    void CocoaWindow::toggleFullscreen()
    {
        // iOS windows are inherently fullscreen; no-op.
    }
}
