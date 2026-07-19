#ifndef WATERFALL_PLOT_WIDGET_H
#define WATERFALL_PLOT_WIDGET_H

#include "room_correction/FrequencyResponse.h"
#include "room_correction/ImpulseResponse.h"

#include <QComboBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QWidget>
#include <optional>
#include <vector>

class WaterfallPlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit WaterfallPlotWidget(QWidget* parent = nullptr);
    ~WaterfallPlotWidget() override;

    void setSlices(const std::vector<std::pair<double, FrequencyResponse>>& slices);
    void recomputeSTFTAsync(const ImpulseResponse& ir, int sliceCount = 30, double maxTimeMs = 400.0,
                            int windowLength = 2048);

    void setFloorDB(double floorDB);
    void setFrequencyBounds(double fMin, double fMax);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double m_floorDB = -40.0;
    double m_fMin = 20.0;
    double m_fMax = 1000.0;
    int m_sliceCount = 30;
    double m_maxTimeMs = 400.0;
    int m_windowLength = 2048;
    bool m_isComputing = false;

    std::optional<ImpulseResponse> m_ir;
    std::vector<std::pair<double, FrequencyResponse>> m_slices;
    QFutureWatcher<std::vector<std::pair<double, FrequencyResponse>>> m_watcher;

    QComboBox* m_timeCombo = nullptr;
    QComboBox* m_slicesCombo = nullptr;
    QComboBox* m_windowCombo = nullptr;
    QComboBox* m_floorCombo = nullptr;
    QPushButton* m_exportBtn = nullptr;

    void setupControlsBar();
    void triggerRecompute();
    void onExport();
};

#endif // WATERFALL_PLOT_WIDGET_H
