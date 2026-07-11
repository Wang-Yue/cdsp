#ifndef SPECTRUM_VIEW_H
#define SPECTRUM_VIEW_H

#include "config/DSPConfigTypes.h"
#include "models/SpectrumEngine.h"
#include <QWidget>
#include <QPainter>
#include <memory>

class SpectrumView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumView(QWidget* parent = nullptr);
    explicit SpectrumView(std::shared_ptr<SpectrumEngine> engine, QWidget* parent = nullptr);

    void setEngine(std::shared_ptr<SpectrumEngine> engine);
    void setSpectrum(const SpectrumData& data);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    std::shared_ptr<SpectrumEngine> m_engine;
    SpectrumData m_data;

    QPoint m_hoverPos;
    bool m_isHovered = false;
};

#endif // SPECTRUM_VIEW_H
