#include "utils/MacUtils.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <QWidget>
#include <QMetaObject>

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

void disableFullScreen(QWidget* widget) {
    if (!widget)
        return;

    WId winId = widget->winId();
    NSView* nsView = (__bridge NSView*)reinterpret_cast<void*>(winId);
    if (!nsView)
        return;

    NSWindow* nsWindow = [nsView window];
    if (!nsWindow)
        return;

    NSWindowCollectionBehavior behavior = [nsWindow collectionBehavior];
    behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
    behavior &= ~NSWindowCollectionBehaviorFullScreenAuxiliary;
    behavior |= NSWindowCollectionBehaviorFullScreenNone;
    [nsWindow setCollectionBehavior:behavior];
}

void showDockIcon() {
    if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    }
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void hideDockIcon() {
    if ([NSApp activationPolicy] != NSApplicationActivationPolicyAccessory) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

} // namespace MacUtils
#endif
