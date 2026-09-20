#ifndef GROUP_DELAY_PLOT_WIDGET_H
#define GROUP_DELAY_PLOT_WIDGET_H

#include "room_correction/FrequencyResponse.h"  // for FrequencyResponse
#include "room_correction/MeasurementSession.h" // for MeasurementSession

#include <QObject>     // for Q_OBJECT
#include <QPushButton> // for QPushButton
#include <QWidget>     // for QWidget
#include <vector>      // for vector

class GroupDelayPlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit GroupDelayPlotWidget(QWidget* parent = nullptr);

    void setSession(MeasurementSession* session);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    MeasurementSession* m_session = nullptr;
    QPushButton* m_exportBtn = nullptr;

    double freqToX(double f, double width) const;
    double autoScaleMs(const FrequencyResponse& fr, const std::vector<double>& gd) const;
    void onExport();
};

#endif // GROUP_DELAY_PLOT_WIDGET_H
