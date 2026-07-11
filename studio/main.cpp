#include "ui/MainWindow.h"
#include "utils/CrashHandler.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QSlider>
#include <QSurfaceFormat>
#include <algorithm>
#include <cmath>

class ClickableSliderFilter : public QObject {
public:
    explicit ClickableSliderFilter(QObject* parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        auto slider = qobject_cast<QSlider*>(obj);
        if (slider && slider->isEnabled()) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton) {
                    double val = 0;
                    if (slider->orientation() == Qt::Horizontal) {
                        double frac = static_cast<double>(me->pos().x()) / std::max(1, slider->width());
                        val = slider->minimum() + frac * (slider->maximum() - slider->minimum());
                    } else {
                        double frac =
                            static_cast<double>(slider->height() - me->pos().y()) / std::max(1, slider->height());
                        val = slider->minimum() + frac * (slider->maximum() - slider->minimum());
                    }
                    slider->setValue(static_cast<int>(std::round(val)));
                }
            }
        }
        return QObject::eventFilter(obj, event);
    }
};

int main(int argc, char* argv[]) {
    installCrashHandler();
    QApplication app(argc, argv);
    app.setApplicationName("CamillaDSP Monitor - Qt");
    app.setOrganizationName("DSPMonitor");
    app.setOrganizationDomain("dspmonitor.io");
    app.installEventFilter(new ClickableSliderFilter(&app));

    // Enable high DPI scaling
    QSurfaceFormat format;
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    MainWindow window;
    window.show();

    return app.exec();
}
