#ifndef APP_ICON_H
#define APP_ICON_H

#include <QIcon>
#include <QPixmap>

namespace AppIcon {

/**
 * Generates an application / tray icon pixmap at the specified size with anti-aliasing.
 */
QPixmap createIconPixmap(int size);

/**
 * Returns a multi-resolution QIcon containing pixmaps for all common DPI scales
 * (16x16, 20x20, 24x24, 32x32, 48x48, 64x64, 128x128, 256x256).
 */
QIcon getAppIcon();

} // namespace AppIcon

#endif // APP_ICON_H
