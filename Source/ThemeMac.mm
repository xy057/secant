#include "Theme.h"
#import <AppKit/AppKit.h>

namespace ui
{
// the app-wide appearance: title bars, menus and file dialogs follow the chosen theme
void setNativeAppearance (int mode)
{
    NSApp.appearance = mode == 1 ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
                     : mode == 2 ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
                                 : nil;
}
} // namespace ui
