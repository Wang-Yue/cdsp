#ifndef SPECTROGRAM_VIEW_H
#define SPECTROGRAM_VIEW_H

#include "models/SpectrogramEngine.h"
#include <QWidget>
#include <QPainter>
#include <memory>

class SpectrogramView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget* parent = nullptr);
    explicit SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent = nullptr);

    void setEngine(std::shared_ptr<SpectrogramEngine> engine);
    void setHistory(const std::deque<SpectrumData>& history, bool show3D = false);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<SpectrogramEngine> m_engine;
    std::deque<SpectrumData> m_history;
    bool m_show3D = false;

    static QColor colorForDB(float db);
};

#endif // SPECTROGRAM_VIEW_H
