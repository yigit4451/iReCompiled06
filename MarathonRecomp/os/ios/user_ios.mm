#include <os/user.h>

#import <UIKit/UIKit.h>

bool os::user::IsDarkTheme()
{
    @autoreleasepool
    {
        return [UITraitCollection currentTraitCollection].userInterfaceStyle == UIUserInterfaceStyleDark;
    }
}
