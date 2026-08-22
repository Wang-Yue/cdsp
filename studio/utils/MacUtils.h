#pragma once

#include <functional>

class QWidget;

namespace MacUtils {

#ifdef __APPLE__
void setupAlwaysOnTopAboveFullScreen(QWidget* widget);
void disableFullScreen(QWidget* widget);
void showAndActivate(QWidget* widget);
void setupMinimizeToTray(QWidget* widget, std::function<bool()> isEnabled);
void setupDockClickHandler(QWidget* widget);
void setDockIconVisible(bool visible);
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
inline void setupMinimizeToTray(QWidget* /*widget*/, std::function<bool()> /*isEnabled*/) {}
inline void setupDockClickHandler(QWidget* /*widget*/) {}
inline void setDockIconVisible(bool /*visible*/) {}
#endif

} // namespace MacUtils
