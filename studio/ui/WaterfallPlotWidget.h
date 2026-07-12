#ifndef WATERFALL_PLOT_WIDGET_H
#define WATERFALL_PLOT_WIDGET_H

#include "room_correction/FrequencyResponse.h"
#include "room_correction/ImpulseResponse.h"

#include <QFutureWatcher>
#include <QPainter>
#include <QWidget>
#include <vector>

class WaterfallPlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit WaterfallPlotWidget(QWidget* parent = nullptr);
    ~WaterfallPlotWidget() override;

    void setSlices(const std::vector<std::pair<double, FrequencyResponse>>& slices);
    void recomputeSTFTAsync(const ImpulseResponse& ir, int sliceCount = 30, double maxTimeMs = 400.0,
                            int windowLength = 2048);

    void setFloorDB(double floorDB) {
        m_floorDB = floorDB;
        update();
    }
    void setFrequencyBounds(double fMin, double fMax) {
        m_fMin = fMin;
        m_fMax = fMax;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double m_floorDB = -40.0;
    double m_fMin = 20.0;
    double m_fMax = 1000.0;
    std::vector<std::pair<double, FrequencyResponse>> m_slices;
    QFutureWatcher<std::vector<std::pair<double, FrequencyResponse>>> m_watcher;
};

#endif // WATERFALL_PLOT_WIDGET_H
