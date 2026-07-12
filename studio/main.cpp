#include "ui/MainWindow.h"
#include "utils/CrashHandler.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
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
                    int val = 0;
                    if (slider->orientation() == Qt::Horizontal) {
                        val = QStyle::sliderValueFromPosition(slider->minimum(), slider->maximum(), me->pos().x(),
                                                              slider->width(), slider->invertedAppearance());
                    } else {
                        val = QStyle::sliderValueFromPosition(slider->minimum(), slider->maximum(),
                                                              slider->height() - me->pos().y(), slider->height(),
                                                              slider->invertedAppearance());
                    }
                    slider->setValue(val);
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
    app.setQuitOnLastWindowClosed(false);
    app.installEventFilter(new ClickableSliderFilter(&app));

    // Enable high DPI scaling
    QSurfaceFormat format;
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    MainWindow window;
    window.show();

    return app.exec();
}
