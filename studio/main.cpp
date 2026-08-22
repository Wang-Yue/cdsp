#include "ui/MainWindow.h"
#include "utils/AppIcon.h"
#include "utils/MacUtils.h"
#include "utils/ThemeManager.h"

#include <QApplication>
#include <QSurfaceFormat>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("CDSP Monitor - Qt");
    app.setOrganizationName("DSPMonitor");
    app.setOrganizationDomain("dspmonitor.io");
    app.setDesktopFileName("com.wangyue.monitorqt");
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
    window.show();

    return app.exec();
}
