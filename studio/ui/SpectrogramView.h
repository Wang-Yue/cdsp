#ifndef SPECTROGRAM_VIEW_H
#define SPECTROGRAM_VIEW_H

#include "models/SpectrogramEngine.h"

#include <QDateTime>
#include <QOpenGLWidget>
#include <QPainter>
#include <deque>
#include <memory>

class SpectrogramView : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget* parent = nullptr);
    explicit SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent = nullptr);
    ~SpectrogramView() override;

    void setEngine(std::shared_ptr<SpectrogramEngine> engine);
    void setHistory(const std::deque<SpectrumData>& history, bool show3D = false,
                    ColorPalette palette = ColorPalette::Classic);

protected:
    void paintGL() override;

    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void paint2D(QPainter& p, int w, int h);
    void paint3D(QPainter& p, int w, int h);

    static QColor colorForDB(float db, ColorPalette palette = ColorPalette::Classic);

    std::shared_ptr<SpectrogramEngine> m_engine;
    std::deque<SpectrumData> m_history;
    bool m_show3D = false;
    ColorPalette m_palette = ColorPalette::Classic;
};

#endif // SPECTROGRAM_VIEW_H
