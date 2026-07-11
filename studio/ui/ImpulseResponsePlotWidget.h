#ifndef IMPULSE_RESPONSE_PLOT_WIDGET_H
#define IMPULSE_RESPONSE_PLOT_WIDGET_H

#include "room_correction/MeasurementSession.h"

#include <QWidget>

class ImpulseResponsePlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit ImpulseResponsePlotWidget(QWidget* parent = nullptr);

    void setSession(MeasurementSession* session);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MeasurementSession* m_session = nullptr;
};

#endif // IMPULSE_RESPONSE_PLOT_WIDGET_H
