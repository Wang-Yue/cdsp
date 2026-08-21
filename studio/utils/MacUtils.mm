#include "utils/MacUtils.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <QWidget>

namespace MacUtils {

void setupAlwaysOnTopAboveFullScreen(QWidget* widget) {
    if (!widget)
        return;

    // Ensure native window exists and get NSWindow
    WId winId = widget->winId();
    NSView* nsView = (__bridge NSView*)reinterpret_cast<void*>(winId);
    if (!nsView)
        return;

    NSWindow* nsWindow = [nsView window];
    if (!nsWindow)
        return;

    nsWindow.hidesOnDeactivate = NO;
    nsWindow.level = NSScreenSaverWindowLevel;
    NSUInteger collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary |
                                    NSWindowCollectionBehaviorStationary;
    nsWindow.collectionBehavior = collectionBehavior;
}

} // namespace MacUtils
#endif
