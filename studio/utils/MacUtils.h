#pragma once

#include <functional>

class QWidget;

namespace MacUtils {

#ifdef __APPLE__
void setupAlwaysOnTopAboveFullScreen(QWidget* widget);
void disableFullScreen(QWidget* widget);
void setupMinimizeToTray(QWidget* widget);
void showDockIcon();
void hideDockIcon();
#else
inline void setupAlwaysOnTopAboveFullScreen(QWidget* /*widget*/) {}
inline void disableFullScreen(QWidget* /*widget*/) {}
inline void setupMinimizeToTray(QWidget* /*widget*/) {}
inline void showDockIcon() {}
inline void hideDockIcon() {}
#endif

} // namespace MacUtils
