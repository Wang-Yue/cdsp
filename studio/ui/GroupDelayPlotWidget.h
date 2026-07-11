#ifndef GROUP_DELAY_PLOT_WIDGET_H
#define GROUP_DELAY_PLOT_WIDGET_H

#include "room_correction/MeasurementSession.h"

#include <QWidget>

class GroupDelayPlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit GroupDelayPlotWidget(QWidget* parent = nullptr);

    void setSession(MeasurementSession* session);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MeasurementSession* m_session = nullptr;

    double freqToX(double f, double width) const;
    double autoScaleMs(const FrequencyResponse& fr, const std::vector<double>& gd) const;
};

#endif // GROUP_DELAY_PLOT_WIDGET_H
