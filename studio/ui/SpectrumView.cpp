#include "ui/SpectrumView.h"
#include "ui/StyleTheme.h"
#include <cmath>

SpectrumView::SpectrumView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

SpectrumView::SpectrumView(std::shared_ptr<SpectrumEngine> engine, QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(180);
    setEngine(engine);
}

void SpectrumView::setEngine(std::shared_ptr<SpectrumEngine> engine) {
    m_engine = engine;
    if (m_engine) {
        connect(m_engine.get(), &SpectrumEngine::updated, this, [this]() {
            if (m_engine) setSpectrum(m_engine->data);
        });
        setSpectrum(m_engine->data);
    }
}

void SpectrumView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_engine) m_engine->visibilityCount++;
}

void SpectrumView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0) m_engine->visibilityCount--;
}

void SpectrumView::setSpectrum(const SpectrumData& data) {
    m_data = data;
    update();
}

void SpectrumView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    int w = width();
    int h = height();

    // Grid lines
    p.setPen(QPen(QColor("#2e2e38"), 1, Qt::DashLine));
    for (double db : {-60, -40, -20, 0}) {
        double y = h - (db + 80.0) / 80.0 * h;
        p.drawLine(0, y, w, y);
    }

    if (m_data.frequencies.empty()) return;

    size_t count = m_data.frequencies.size();
    int barW = std::max(2, w / static_cast<int>(count));

    for (size_t i = 0; i < count; ++i) {
        float db = m_data.magnitudes[i];
        float norm = std::max(0.0f, std::min(1.0f, (db + 80.0f) / 80.0f));
        int barH = static_cast<int>(norm * h);

        int x = static_cast<int>(i) * barW;
        int y = h - barH;

        QLinearGradient grad(x, y, x, h);
        grad.setColorAt(0.0, QColor("#007af5"));
        grad.setColorAt(1.0, QColor("#2cb67d"));

        p.fillRect(x, y, barW - 1, barH, grad);
    }
}
