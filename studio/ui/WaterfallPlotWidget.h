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
    void recomputeSTFTAsync(const ImpulseResponse& ir, int sliceCount = 30, double maxTimeMs = 500.0,
                            int windowLength = 2048);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<std::pair<double, FrequencyResponse>> m_slices;
    QFutureWatcher<std::vector<std::pair<double, FrequencyResponse>>> m_watcher;
};

#endif // WATERFALL_PLOT_WIDGET_H
