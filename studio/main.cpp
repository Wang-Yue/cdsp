#include "ui/MainWindow.h"
#include "utils/CrashHandler.h"

#include <QApplication>
#include <QProxyStyle>
#include <QSurfaceFormat>

class AppStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override {
        if (hint == SH_Slider_AbsoluteSetButtons) {
            return Qt::LeftButton | Qt::MiddleButton;
        }
        if (hint == SH_Slider_PageSetButtons) {
            return Qt::NoButton;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

int main(int argc, char* argv[]) {
    installCrashHandler();
    QApplication app(argc, argv);
    app.setStyle(new AppStyle(app.style()));
    app.setApplicationName("CamillaDSP Monitor - Qt");
    app.setOrganizationName("DSPMonitor");
    app.setOrganizationDomain("dspmonitor.io");
    app.setQuitOnLastWindowClosed(false);

    // Enable high DPI scaling
    QSurfaceFormat format;
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    MainWindow window;
    window.show();

    return app.exec();
}
