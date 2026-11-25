#include <os/locale.h>
#include <CoreFoundation/CoreFoundation.h>

std::string os::locale::ConvertToUTF8(Encoding in_encoding, const std::string& in_rStr)
{
    if (in_rStr.empty())
        return {};

    CFStringEncoding cfEncoding;

    switch (in_encoding)
    {
        case Encoding::ShiftJIS:
            cfEncoding = kCFStringEncodingDOSJapanese;
            break;

        default:
            assert(false && "Unimplemented.");
            return {};
    }

    CFDataRef cfData = CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8*>(in_rStr.data()),
        static_cast<CFIndex>(in_rStr.size()), kCFAllocatorNull);
    if (!cfData)
    {
        assert(false && "CFDataCreateWithBytesNoCopy failed");
        return {};
    }

    CFStringRef cfStr = CFStringCreateFromExternalRepresentation(nullptr, cfData, cfEncoding);
    CFRelease(cfData);
    if (!cfStr)
    {
        assert(false && "CFStringCreateFromExternalRepresentation failed");
        return {};
    }

    CFDataRef cfConvertedData = CFStringCreateExternalRepresentation(nullptr, cfStr, kCFStringEncodingUTF8, 0);
    CFRelease(cfStr);
    if (!cfConvertedData)
    {
        assert(false && "CFStringCreateExternalRepresentation failed");
        return {};
    }

    std::string result(reinterpret_cast<const char*>(CFDataGetBytePtr(cfConvertedData)), CFDataGetLength(cfConvertedData));
    CFRelease(cfConvertedData);

    return result;
}
