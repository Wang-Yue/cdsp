#ifndef SPECTROGRAM_VIEW_H
#define SPECTROGRAM_VIEW_H

#include "models/SpectrogramEngine.h"

#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QWidget>
#include <memory>

class SpectrogramView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget* parent = nullptr);
    explicit SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent = nullptr);

    void setEngine(std::shared_ptr<SpectrogramEngine> engine);
    void setHistory(const std::deque<SpectrumData>& history, bool show3D = false,
                    ColorPalette palette = ColorPalette::Classic);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<SpectrogramEngine> m_engine;
    std::deque<SpectrumData> m_history;
    bool m_show3D = false;
    ColorPalette m_palette = ColorPalette::Classic;

    QImage m_bufferImage;
    double m_currentX = 0.0;
    QDateTime m_lastUpdateTime;

    void recreateBuffer(const QSize& size, const std::deque<SpectrumData>& history);
    void redrawAllHistory(QImage& bufferImage, const std::deque<SpectrumData>& history, const QSize& size,
                          int drawWidth, int drawHeight);
    void updateBuffer(const std::deque<SpectrumData>& history, const QSize& size, double elapsedSeconds);
    void drawFrame(QPainter& painter, const SpectrumData& frame, double x, double width, double drawHeight,
                   size_t nBins);

    static QColor colorForDB(float db, ColorPalette palette = ColorPalette::Classic);
};

#endif // SPECTROGRAM_VIEW_H
