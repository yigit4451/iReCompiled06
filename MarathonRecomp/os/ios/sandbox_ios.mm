#include <os/ios/sandbox_ios.h>
#include <os/ios/memory_ios.h>

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <cstdlib>
#include <mutex>

@interface MarathonDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, assign) dispatch_semaphore_t completionSemaphore;
@property (nonatomic, copy) NSArray<NSURL*>* selectedURLs;
@property (nonatomic, assign) BOOL wasCancelled;
@end

@implementation MarathonDocumentPickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    self.selectedURLs = urls;
    self.wasCancelled = NO;
    if (self.completionSemaphore)
        dispatch_semaphore_signal(self.completionSemaphore);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
    self.selectedURLs = @[];
    self.wasCancelled = YES;
    if (self.completionSemaphore)
        dispatch_semaphore_signal(self.completionSemaphore);
}

@end

namespace
{
    constexpr const char* kAppSupportSubdir = "iReCompiled06";

    std::filesystem::path ResolveSearchPath(NSSearchPathDirectory directory, bool appendAppSubdir, bool excludeFromBackup)
    {
        @autoreleasepool
        {
            NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES);
            std::filesystem::path basePath;

            if (paths.count > 0 && paths.firstObject.length > 0)
            {
                basePath = std::filesystem::path([paths.firstObject fileSystemRepresentation]);
            }
            else if (const char* home = std::getenv("HOME"))
            {
                std::filesystem::path homePath(home);
                if (directory == NSDocumentDirectory)
                    basePath = homePath / "Documents";
                else if (directory == NSCachesDirectory)
                    basePath = homePath / "Library" / "Caches";
                else
                    basePath = homePath / "Library" / "Application Support";
            }

            if (appendAppSubdir && !basePath.empty())
                basePath /= kAppSupportSubdir;

            if (!basePath.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(basePath, ec);

                if (excludeFromBackup)
                {
                    NSString* nsPath = [NSString stringWithUTF8String:basePath.c_str()];
                    if (nsPath)
                    {
                        NSURL* dirURL = [NSURL fileURLWithPath:nsPath isDirectory:YES];
                        [dirURL setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
                    }
                }
            }

            return basePath;
        }
    }

    UIViewController* FindTopViewController()
    {
        UIWindow* keyWindow = nil;

        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        {
            if (![scene isKindOfClass:[UIWindowScene class]])
                continue;

            UIWindowScene* windowScene = (UIWindowScene*)scene;
            if (windowScene.activationState == UISceneActivationStateForegroundActive || keyWindow == nil)
            {
                for (UIWindow* window in windowScene.windows)
                {
                    if (window.isKeyWindow)
                    {
                        keyWindow = window;
                        break;
                    }
                }
                if (!keyWindow && windowScene.windows.count > 0)
                    keyWindow = windowScene.windows.firstObject;
            }
        }

        UIViewController* topController = keyWindow.rootViewController;
        while (topController.presentedViewController != nil)
            topController = topController.presentedViewController;

        return topController;
    }
}

const std::filesystem::path& os::ios::GetDocumentsPath()
{
    static const std::filesystem::path s_documentsPath = ResolveSearchPath(NSDocumentDirectory, false, false);
    return s_documentsPath;
}

const std::filesystem::path& os::ios::GetGameInstallPath()
{
    static const std::filesystem::path s_gameInstallPath = ResolveSearchPath(NSDocumentDirectory, true, false);
    std::error_code ec;
    std::filesystem::create_directories(s_gameInstallPath, ec);
    return s_gameInstallPath;
}

const std::filesystem::path& os::ios::GetApplicationSupportPath()
{
    static const std::filesystem::path s_appSupportPath = ResolveSearchPath(NSApplicationSupportDirectory, true, true);
    return s_appSupportPath;
}

const std::filesystem::path& os::ios::GetCachesPath()
{
    static const std::filesystem::path s_cachesPath = ResolveSearchPath(NSCachesDirectory, true, true);
    return s_cachesPath;
}

const std::filesystem::path& os::ios::GetBundleResourcePath()
{
    static const std::filesystem::path s_bundleResourcePath = []() -> std::filesystem::path
    {
        @autoreleasepool
        {
            NSString* resourcePath = [[NSBundle mainBundle] resourcePath];
            if (resourcePath && resourcePath.length > 0)
                return std::filesystem::path([resourcePath fileSystemRepresentation]);

            return {};
        }
    }();
    return s_bundleResourcePath;
}

void os::ios::EnsureSandboxDirectories()
{
    std::error_code ec;
    const auto& gameRoot = GetGameInstallPath();
    const auto& appSupport = GetApplicationSupportPath();
    const auto& caches = GetCachesPath();

    // User-facing directories inside <Sandbox>/Documents/iReCompiled06 (visible in iOS Files app)
    std::filesystem::create_directories(gameRoot / "game", ec);
    std::filesystem::create_directories(gameRoot / "decrypted", ec);
    std::filesystem::create_directories(gameRoot / "dlc", ec);
    std::filesystem::create_directories(gameRoot / "mods", ec);
    std::filesystem::create_directories(gameRoot / "save", ec);

    // Internal cache/support directories inside <Sandbox>/Library/Application Support/iReCompiled06
    const auto mvkCacheDir = appSupport / "cache";
    std::filesystem::create_directories(mvkCacheDir, ec);
    std::filesystem::create_directories(caches, ec);

    // Direct MoltenVK shader/pipeline caches into Library/Application Support if not already configured
    if (!mvkCacheDir.empty())
        setenv("MVK_CONFIG_AUTO_GPU_CAPTURE_OUTPUT_FILE", (appSupport / "mvk_capture.gputrace").c_str(), 0);

    // Apply 4 GB - 6 GB iOS Jetsam pool limits for MoltenVK and install memory-pressure observers
    ConfigureMoltenVKMemoryPool();
    InstallMemoryWarningObserver();
}

void os::ios::OpenURL(std::string_view url)
{
    if (url.empty())
        return;

    std::string urlCopy(url);
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool
        {
            NSString* urlStr = [[NSString alloc] initWithBytes:urlCopy.data()
                                                        length:urlCopy.size()
                                                      encoding:NSUTF8StringEncoding];
            if (!urlStr)
                return;

            NSURL* nsUrl = [NSURL URLWithString:urlStr];
#if !__has_feature(objc_arc)
            [urlStr release];
#endif
            if (nsUrl)
                [[UIApplication sharedApplication] openURL:nsUrl options:@{} completionHandler:nil];
        }
    });
}

bool os::ios::FetchURLText(std::string_view url, std::string_view userAgent, std::string& outBody, std::string& outError)
{
    @autoreleasepool
    {
        NSString* urlStr = [[NSString alloc] initWithBytes:url.data() length:url.size() encoding:NSUTF8StringEncoding];
        if (!urlStr)
        {
            outError = "Invalid UTF-8 URL string.";
            return false;
        }

        NSURL* nsUrl = [NSURL URLWithString:urlStr];
#if !__has_feature(objc_arc)
        [urlStr release];
#endif
        if (!nsUrl)
        {
            outError = "Malformed URL.";
            return false;
        }

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsUrl
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:15.0];
        [req setHTTPMethod:@"GET"];
        if (!userAgent.empty())
        {
            NSString* uaStr = [[NSString alloc] initWithBytes:userAgent.data() length:userAgent.size() encoding:NSUTF8StringEncoding];
            if (uaStr)
            {
                [req setValue:uaStr forHTTPHeaderField:@"User-Agent"];
#if !__has_feature(objc_arc)
                [uaStr release];
#endif
            }
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block BOOL success = NO;
        __block std::string bodyResult;
        __block std::string errResult;

        NSURLSessionDataTask* task = [[NSURLSession sharedSession]
            dataTaskWithRequest:req
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                  if (error != nil)
                  {
                      const char* desc = error.localizedDescription.UTF8String;
                      errResult = desc ? desc : "NSURLSession request failed.";
                  }
                  else if ([response isKindOfClass:[NSHTTPURLResponse class]])
                  {
                      NSInteger status = ((NSHTTPURLResponse*)response).statusCode;
                      if (status >= 200 && status < 300 && data != nil)
                      {
                          bodyResult.assign(static_cast<const char*>(data.bytes), data.length);
                          success = YES;
                      }
                      else
                      {
                          errResult = "HTTP status " + std::to_string(static_cast<long>(status));
                      }
                  }
                  else if (data != nil)
                  {
                      bodyResult.assign(static_cast<const char*>(data.bytes), data.length);
                      success = YES;
                  }
                  dispatch_semaphore_signal(sem);
              }];
        [task resume];

        const dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 20LL * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(sem, timeout) != 0)
        {
            [task cancel];
            outError = "Request timed out.";
            return false;
        }

        if (success)
        {
            outBody = std::move(bodyResult);
            return true;
        }

        outError = errResult.empty() ? "Unknown network error." : std::move(errResult);
        return false;
    }
}

bool os::ios::PickFiles(bool folderMode, std::list<std::filesystem::path>& outPaths, std::string& outError)
{
    if ([NSThread isMainThread])
    {
        outError = "os::ios::PickFiles must be called from a background worker thread.";
        return false;
    }

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block MarathonDocumentPickerDelegate* delegate = nil;
    __block BOOL presented = NO;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool
        {
            UIViewController* presenter = FindTopViewController();
            if (!presenter)
            {
                dispatch_semaphore_signal(sem);
                return;
            }

            delegate = [[MarathonDocumentPickerDelegate alloc] init];
            delegate.completionSemaphore = sem;

            NSArray<UTType*>* contentTypes = folderMode ? @[UTTypeFolder] : @[UTTypeItem, UTTypeData, UTTypeArchive];
            UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:contentTypes
                                                                                                                 asCopy:NO];
            picker.allowsMultipleSelection = YES;
            picker.delegate = delegate;

            presented = YES;
            [presenter presentViewController:picker animated:YES completion:nil];
#if !__has_feature(objc_arc)
            [picker release];
#endif
        }
    });

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    @autoreleasepool
    {
        if (!presented || !delegate)
        {
            outError = "Unable to locate an active UIViewController to present UIDocumentPickerViewController.";
            return false;
        }

        BOOL ok = NO;
        if (!delegate.wasCancelled && delegate.selectedURLs.count > 0)
        {
            for (NSURL* url in delegate.selectedURLs)
            {
                [url startAccessingSecurityScopedResource];
                if (url.fileSystemRepresentation)
                    outPaths.emplace_back(url.fileSystemRepresentation);
            }
            ok = !outPaths.empty();
        }

#if !__has_feature(objc_arc)
        [delegate release];
#endif
        return ok;
    }
}

void os::ios::ConfigureGameControllerSupport(bool monitorBackgroundEvents)
{
    auto applyBlock = ^{
        @autoreleasepool
        {
            if (@available(iOS 14.5, *))
                GCController.shouldMonitorBackgroundEvents = monitorBackgroundEvents ? YES : NO;

            // Ensure wireless MFi / DualSense / Xbox discovery is active without blocking UI
            [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
        }
    };

    if ([NSThread isMainThread])
        applyBlock();
    else
        dispatch_async(dispatch_get_main_queue(), applyBlock);
}
