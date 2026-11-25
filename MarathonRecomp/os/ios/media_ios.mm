#include <os/media.h>

#import <AVFAudio/AVFAudio.h>

bool os::media::IsExternalMediaPlaying()
{
    @autoreleasepool
    {
        return [[AVAudioSession sharedInstance] isOtherAudioPlaying] == YES;
    }
}
