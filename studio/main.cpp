#include "ui/MainWindow.h"
#include "utils/AppIcon.h"

#include <QApplication>
#include <QSurfaceFormat>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("CDSP Monitor - Qt");
    app.setOrganizationName("DSPMonitor");
    app.setOrganizationDomain("dspmonitor.io");
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(AppIcon::getAppIcon());

    // Enable high DPI scaling
    QSurfaceFormat format;
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    MainWindow window;
    window.setWindowIcon(AppIcon::getAppIcon());
    window.show();

    return app.exec();
}
