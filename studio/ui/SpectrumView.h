#ifndef SPECTRUM_VIEW_H
#define SPECTRUM_VIEW_H

#include "config/DSPConfigTypes.h" // for SpectrumData
#include "models/SpectrumEngine.h" // for OctaveSmoothing, SpectrumEngine

#include <QObject> // for Q_OBJECT
#include <QPoint>  // for QPoint
#include <QWidget> // for QWidget
#include <memory>  // for shared_ptr
#include <vector>  // for vector

class SpectrumView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumView(QWidget* parent = nullptr);
    explicit SpectrumView(std::shared_ptr<SpectrumEngine> engine, QWidget* parent = nullptr);
    ~SpectrumView() override;

    void setEngine(std::shared_ptr<SpectrumEngine> engine);
    void setSpectrum(const SpectrumData& data, OctaveSmoothing smoothing = OctaveSmoothing::None,
                     float peakHoldDecayRate = 0.95f);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    std::shared_ptr<SpectrumEngine> m_engine;
    SpectrumData m_data;
    std::vector<float> m_peakHold;
    OctaveSmoothing m_smoothing = OctaveSmoothing::None;
    float m_peakHoldDecayRate = 0.95f;

    QPoint m_hoverPos;
    bool m_isHovered = false;
};

#endif // SPECTRUM_VIEW_H
