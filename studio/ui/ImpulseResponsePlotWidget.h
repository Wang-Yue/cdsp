#ifndef IMPULSE_RESPONSE_PLOT_WIDGET_H
#define IMPULSE_RESPONSE_PLOT_WIDGET_H

#include "room_correction/MeasurementSession.h" // for MeasurementSession

#include <QObject>     // for Q_OBJECT
#include <QPushButton> // for QPushButton
#include <QWidget>     // for QWidget

class ImpulseResponsePlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit ImpulseResponsePlotWidget(QWidget* parent = nullptr);

    void setSession(MeasurementSession* session);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    MeasurementSession* m_session = nullptr;
    QPushButton* m_exportBtn = nullptr;

    void onExport();
};

#endif // IMPULSE_RESPONSE_PLOT_WIDGET_H
