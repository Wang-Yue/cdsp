#include "ui/MainWindow.h"      // for MainWindow
#include "utils/AppIcon.h"      // for getAppIcon
#include "utils/ThemeManager.h" // for ThemeManager

#include <QApplication>   // for QApplication
#include <QSurfaceFormat> // for QSurfaceFormat

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("CDSP Studio");
    app.setOrganizationName("CDSP");
    app.setOrganizationDomain("cdsp.io");
    app.setDesktopFileName("com.wangyue.cdspstudio");
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(AppIcon::getAppIcon());

    // Enable high DPI scaling
    QSurfaceFormat format;
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    // Initialize ThemeManager to respect system dark/light theme setting
    ThemeManager::init();

    MainWindow window;
    window.setWindowIcon(AppIcon::getAppIcon());
#if defined(__EMSCRIPTEN__)
    // In the browser the page (or the extension's popup window) is the app window: fill it
    // without a Qt-drawn title bar. A maximized window tracks the page size in Qt's wasm
    // platform. (Full-screen is not used: MainWindow::changeEvent reverts it by design.)
    window.setWindowFlag(Qt::FramelessWindowHint);
    window.showMaximized();
#else
    window.show();
#endif

    return app.exec();
}
