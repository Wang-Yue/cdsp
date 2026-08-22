#pragma once

#include <functional>

class QWidget;

namespace MacUtils {

#ifdef __APPLE__
void setupAlwaysOnTopAboveFullScreen(QWidget* widget);
void disableFullScreen(QWidget* widget);
void showAndActivate(QWidget* widget);
void setupMinimizeToTray(QWidget* widget);
void hideDockIcon();
#else
inline void setupAlwaysOnTopAboveFullScreen(QWidget* /*widget*/) {}
inline void disableFullScreen(QWidget* /*widget*/) {}
inline void showAndActivate(QWidget* widget) {
    if (!widget)
        return;
    widget->showNormal();
    widget->raise();
    widget->activateWindow();
}
inline void setupMinimizeToTray(QWidget* /*widget*/) {}
inline void hideDockIcon() {}
#endif

} // namespace MacUtils
