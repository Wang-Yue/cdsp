#pragma once

class QWidget;

namespace MacUtils {

#ifdef __APPLE__
void setupAlwaysOnTopAboveFullScreen(QWidget* widget);
#else
inline void setupAlwaysOnTopAboveFullScreen(QWidget* /*widget*/) {}
#endif

} // namespace MacUtils
