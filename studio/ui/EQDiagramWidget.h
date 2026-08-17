#ifndef EQ_DIAGRAM_WIDGET_H
#define EQ_DIAGRAM_WIDGET_H

#include "models/AudioSettings.h"
#include "models/EQPreset.h"
#include "models/PipelineStore.h"
#include "models/SpectrumEngine.h"
#include "room_correction/TargetCurve.h"

#include <QCheckBox>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QWidget>
#include <functional>

struct EQReferenceOverlayData {
    std::vector<double> frequencies;
    std::vector<double> measuredMagDB;
    TargetCurve targetCurve;
    bool showCorrected = true;
    bool active = false;
};

class EQDiagramWidget : public QWidget {
    Q_OBJECT

public:
    explicit EQDiagramWidget(QWidget* parent = nullptr);
    ~EQDiagramWidget() override;

    void setPreset(const EQPreset& preset, int sampleRate = 48000);
    void setSelectedBandIndex(int index);
    void setSpectrumEngine(std::shared_ptr<SpectrumEngine> spectrum);
    void setAudioSettings(std::shared_ptr<AudioSettings> settings);
    void setReferenceOverlay(const EQReferenceOverlayData& overlay);
    void setPipelineStore(std::shared_ptr<PipelineStore> store);
    void setShowAnalyzer(bool show);
    void setShowLoudnessContour(bool show);

    static QColor bandColor(int index);

    bool showAnalyzer() const { return m_showAnalyzer; }
    bool showLoudnessContour() const { return m_showLoudnessContour; }
    int selectedBandIndex() const { return m_selectedIndex; }

    std::function<void(int index, double freq, double gain)> onBandDragged;
    std::function<void(int index, double q)> onBandQChanged;
    std::function<void(int index)> onBandSelected;
    std::function<void()> onPresetChanged;
    std::function<void(double freq, double gain)> onBandAdded;
    std::function<void(int index)> onBandDeleted;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    EQPreset m_preset;
    int m_sampleRate = 48000;
    int m_selectedIndex = -1;
    int m_draggingIndex = -1;
    int m_hoveredIndex = -1;
    int m_lastDragY = 0;

    bool m_showAnalyzer = true;
    bool m_showLoudnessContour = false;
    std::shared_ptr<SpectrumEngine> m_spectrum;
    std::shared_ptr<PipelineStore> m_pipelineStore;
    std::shared_ptr<AudioSettings> m_audioSettings;
    EQReferenceOverlayData m_overlay;

    double fMin = 20.0;
    double fMax = 20000.0;
    double dbMin = -24.0;
    double dbMax = 24.0;

    double freqToX(double f, double width) const;
    double xToFreq(double x, double width) const;
    double dbToY(double db, double height) const;
    double yToDb(double y, double height) const;
    void drawOverlayReadout(QPainter& painter, int w, int h);
};

#endif // EQ_DIAGRAM_WIDGET_H
