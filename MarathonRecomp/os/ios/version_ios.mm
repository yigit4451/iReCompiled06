#include <os/version.h>

#import <Foundation/Foundation.h>

os::version::OSVersion os::version::GetOSVersion()
{
    @autoreleasepool
    {
        const NSOperatingSystemVersion ver = [[NSProcessInfo processInfo] operatingSystemVersion];
        return {
            static_cast<uint32_t>(ver.majorVersion),
            static_cast<uint32_t>(ver.minorVersion),
            static_cast<uint32_t>(ver.patchVersion)
        };
    }
}
