#ifndef PHASE_PLOT_WIDGET_H
#define PHASE_PLOT_WIDGET_H

#include "room_correction/MeasurementSession.h"
#include <QWidget>
#include <QPushButton>
#include <memory>

class PhasePlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit PhasePlotWidget(QWidget* parent = nullptr);

    void setSession(MeasurementSession* session);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MeasurementSession* m_session = nullptr;
    bool m_unwrapPhase = false;
    QPushButton* m_unwrapBtn = nullptr;

    double freqToX(double f, double width) const;
    void phaseBounds(const FrequencyResponse& fr, const std::vector<double>& unwrapped, double& minDeg, double& maxDeg) const;
    double wrapToPi(double radians) const;
};

#endif // PHASE_PLOT_WIDGET_H
