#ifndef WATERFALL_PLOT_WIDGET_H
#define WATERFALL_PLOT_WIDGET_H

#include "room_correction/FrequencyResponse.h"
#include <QWidget>
#include <QPainter>
#include <vector>

class WaterfallPlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit WaterfallPlotWidget(QWidget* parent = nullptr);

    void setSlices(const std::vector<std::pair<double, FrequencyResponse>>& slices);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<std::pair<double, FrequencyResponse>> m_slices;
};

#endif // WATERFALL_PLOT_WIDGET_H
