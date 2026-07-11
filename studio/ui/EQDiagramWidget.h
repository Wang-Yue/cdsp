#ifndef EQ_DIAGRAM_WIDGET_H
#define EQ_DIAGRAM_WIDGET_H

#include "models/EQPreset.h"
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <functional>

#include "models/SpectrumEngine.h"
#include <QCheckBox>

class EQDiagramWidget : public QWidget {
    Q_OBJECT

public:
    explicit EQDiagramWidget(QWidget* parent = nullptr);

    void setPreset(const EQPreset& preset, int sampleRate = 48000);
    void setSelectedBandIndex(int index);
    void setSpectrumEngine(std::shared_ptr<SpectrumEngine> spectrum) { m_spectrum = spectrum; }
    void setShowAnalyzer(bool show) { m_showAnalyzer = show; update(); }
    void setShowLoudnessContour(bool show) { m_showLoudnessContour = show; update(); }

    bool showAnalyzer() const { return m_showAnalyzer; }
    bool showLoudnessContour() const { return m_showLoudnessContour; }

    std::function<void(int index, double freq, double gain)> onBandDragged;
    std::function<void(int index, double q)> onBandQChanged;
    std::function<void(int index)> onBandSelected;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    EQPreset m_preset;
    int m_sampleRate = 48000;
    int m_selectedIndex = -1;
    int m_draggingIndex = -1;

    bool m_showAnalyzer = true;
    bool m_showLoudnessContour = false;
    std::shared_ptr<SpectrumEngine> m_spectrum;

    double fMin = 20.0;
    double fMax = 20000.0;
    double dbMin = -24.0;
    double dbMax = 24.0;

    double freqToX(double f, double width) const;
    double xToFreq(double x, double width) const;
    double dbToY(double db, double height) const;
    double yToDb(double y, double height) const;
};

#endif // EQ_DIAGRAM_WIDGET_H
