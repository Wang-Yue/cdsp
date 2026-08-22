#include "utils/MacUtils.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <QWidget>
#include <QMetaObject>

@interface TrayMinimizeHandler : NSObject
@property (nonatomic, copy) BOOL (^shouldMinimizeToTray)(void);
@property (nonatomic, copy) void (^onMinimize)(void);
@end

@implementation TrayMinimizeHandler
@end

@interface AppDelegateReopenHandler : NSObject
@property (nonatomic, copy) void (^onReopen)(void);
@end

@implementation AppDelegateReopenHandler
@end

static char kTrayMinimizeHandlerKey;
static char kAppDelegateReopenHandlerKey;
static IMP original_miniaturize = NULL;
static IMP original_performMiniaturize = NULL;
static IMP original_applicationShouldHandleReopen = NULL;

static BOOL swizzled_applicationShouldHandleReopen(id self, SEL _cmd, NSApplication* sender, BOOL hasVisibleWindows) {
    AppDelegateReopenHandler* handler = objc_getAssociatedObject(self, &kAppDelegateReopenHandlerKey);
    if (handler && handler.onReopen && !hasVisibleWindows) {
        handler.onReopen();
        return YES;
    }
    if (original_applicationShouldHandleReopen) {
        return ((BOOL (*)(id, SEL, NSApplication*, BOOL))original_applicationShouldHandleReopen)(self, _cmd, sender, hasVisibleWindows);
    }
    return YES;
}

static void swizzled_miniaturize(id self, SEL _cmd, id sender) {
    TrayMinimizeHandler* handler = objc_getAssociatedObject(self, &kTrayMinimizeHandlerKey);
    if (handler && handler.shouldMinimizeToTray && handler.shouldMinimizeToTray()) {
        [self orderOut:sender];
        MacUtils::setDockIconVisible(false);
        if (handler.onMinimize) {
            handler.onMinimize();
        }
        return;
    }
    if (original_miniaturize) {
        ((void (*)(id, SEL, id))original_miniaturize)(self, _cmd, sender);
    }
}

static void swizzled_performMiniaturize(id self, SEL _cmd, id sender) {
    TrayMinimizeHandler* handler = objc_getAssociatedObject(self, &kTrayMinimizeHandlerKey);
    if (handler && handler.shouldMinimizeToTray && handler.shouldMinimizeToTray()) {
        [self orderOut:sender];
        MacUtils::setDockIconVisible(false);
        if (handler.onMinimize) {
            handler.onMinimize();
        }
        return;
    }
    if (original_performMiniaturize) {
        ((void (*)(id, SEL, id))original_performMiniaturize)(self, _cmd, sender);
    }
}

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

void setDockIconVisible(bool visible) {
    if (visible) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    } else {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

void showAndActivate(QWidget* widget) {
    if (!widget)
        return;

    setDockIconVisible(true);
    [NSApp activateIgnoringOtherApps:YES];
    widget->showNormal();
    widget->raise();
    widget->activateWindow();
}

void setupMinimizeToTray(QWidget* widget, std::function<bool()> isEnabled) {
    if (!widget)
        return;

    WId winId = widget->winId();
    NSView* nsView = (__bridge NSView*)reinterpret_cast<void*>(winId);
    if (!nsView)
        return;

    NSWindow* nsWindow = [nsView window];
    if (!nsWindow)
        return;

    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        Class windowClass = [nsWindow class];

        Method minMethod = class_getInstanceMethod(windowClass, @selector(miniaturize:));
        if (minMethod) {
            original_miniaturize = method_setImplementation(minMethod, (IMP)swizzled_miniaturize);
        }

        Method perfMinMethod = class_getInstanceMethod(windowClass, @selector(performMiniaturize:));
        if (perfMinMethod) {
            original_performMiniaturize = method_setImplementation(perfMinMethod, (IMP)swizzled_performMiniaturize);
        }
    });

    TrayMinimizeHandler* handler = [[TrayMinimizeHandler alloc] init];
    handler.shouldMinimizeToTray = ^{
        return (isEnabled && isEnabled()) ? YES : NO;
    };
    handler.onMinimize = ^{
        QMetaObject::invokeMethod(widget, "hide", Qt::QueuedConnection);
        MacUtils::setDockIconVisible(false);
    };

    objc_setAssociatedObject(nsWindow, &kTrayMinimizeHandlerKey, handler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void setupDockClickHandler(QWidget* widget) {
    if (!widget)
        return;

    id delegate = [NSApp delegate];
    if (!delegate)
        return;

    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        Class delegateClass = [delegate class];
        Method method = class_getInstanceMethod(delegateClass, @selector(applicationShouldHandleReopen:hasVisibleWindows:));
        if (method) {
            original_applicationShouldHandleReopen = method_setImplementation(method, (IMP)swizzled_applicationShouldHandleReopen);
        } else {
            class_addMethod(delegateClass, @selector(applicationShouldHandleReopen:hasVisibleWindows:), (IMP)swizzled_applicationShouldHandleReopen, "c@:@c");
        }
    });

    AppDelegateReopenHandler* handler = [[AppDelegateReopenHandler alloc] init];
    handler.onReopen = ^{
        if (widget->isHidden()) {
            MacUtils::showAndActivate(widget);
        }
    };

    objc_setAssociatedObject(delegate, &kAppDelegateReopenHandlerKey, handler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

} // namespace MacUtils
#endif
