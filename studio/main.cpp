#include "ui/MainWindow.h"
#include "utils/AppIcon.h"

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
    QApplication app(argc, argv);
    app.setStyle(new AppStyle(app.style()));
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
