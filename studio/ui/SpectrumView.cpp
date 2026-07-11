#include "ui/SpectrumView.h"
#include "ui/StyleTheme.h"
#include <QMouseEvent>
#include <QPainterPath>
#include <cmath>

SpectrumView::SpectrumView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setMouseTracking(true);
}

SpectrumView::SpectrumView(std::shared_ptr<SpectrumEngine> engine, QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(180);
    setMouseTracking(true);
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

void SpectrumView::mouseMoveEvent(QMouseEvent* event) {
    m_hoverPos = event->pos();
    m_isHovered = rect().contains(m_hoverPos);
    update();
}

void SpectrumView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    int w = width();
    int h = height();
    int marginB = 20;
    int plotH = h - marginB;

    // dB Grid lines & labels
    p.setFont(QFont("sans-serif", 9));
    p.setPen(QPen(QColor("#2e2e38"), 1, Qt::DashLine));
    for (double db : {-60, -40, -20, 0}) {
        double y = plotH - (db + 80.0) / 80.0 * plotH;
        p.drawLine(0, y, w, y);
        p.setPen(QColor("#8e8e93"));
        p.drawText(4, y - 2, QString("%1 dB").arg(static_cast<int>(db)));
        p.setPen(QPen(QColor("#2e2e38"), 1, Qt::DashLine));
    }

    // Freq Grid lines & labels
    double minF = 20.0, maxF = 20000.0;
    double logMin = std::log10(minF), logMax = std::log10(maxF);

    for (double f : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) {
        double x = (std::log10(f) - logMin) / (logMax - logMin) * w;
        p.setPen(QPen(QColor("#2e2e38"), 1, Qt::DashLine));
        p.drawLine(x, 0, x, plotH);
        p.setPen(QColor("#8e8e93"));
        QString label = f >= 1000.0 ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        p.drawText(x - 10, h - 4, label);
    }

    if (m_data.frequencies.empty()) return;

    size_t count = m_data.frequencies.size();
    QPainterPath curvePath;
    curvePath.moveTo(0, plotH);

    for (size_t i = 0; i < count; ++i) {
        float freq = m_data.frequencies[i];
        if (freq < minF || freq > maxF) continue;

        float db = m_data.magnitudes[i];
        float normY = std::max(0.0f, std::min(1.0f, (db + 80.0f) / 80.0f));
        double y = plotH - normY * plotH;
        double x = (std::log10(freq) - logMin) / (logMax - logMin) * w;

        if (i == 0) curvePath.moveTo(x, y);
        else curvePath.lineTo(x, y);
    }

    QLinearGradient grad(0, 0, 0, plotH);
    grad.setColorAt(0.0, QColor(0, 122, 245, 180));
    grad.setColorAt(1.0, QColor(44, 182, 125, 40));

    QPainterPath fillPath = curvePath;
    fillPath.lineTo(w, plotH);
    fillPath.lineTo(0, plotH);
    fillPath.closeSubpath();

    p.fillPath(fillPath, grad);
    p.setPen(QPen(QColor("#007af5"), 2));
    p.drawPath(curvePath);

    // Hover readout
    if (m_isHovered && m_hoverPos.x() >= 0 && m_hoverPos.x() <= w) {
        double mouseNormX = static_cast<double>(m_hoverPos.x()) / w;
        double targetFreq = std::pow(10.0, logMin + mouseNormX * (logMax - logMin));

        p.setPen(QPen(QColor("#ff9500"), 1, Qt::SolidLine));
        p.drawLine(m_hoverPos.x(), 0, m_hoverPos.x(), plotH);

        // Find nearest magnitude bin
        float nearestDB = -80.0f;
        double minDiff = 1e9;
        for (size_t i = 0; i < count; ++i) {
            double diff = std::abs(m_data.frequencies[i] - targetFreq);
            if (diff < minDiff) {
                minDiff = diff;
                nearestDB = m_data.magnitudes[i];
            }
        }

        QString tooltip = QString("%1 Hz  |  %2 dB").arg(static_cast<int>(targetFreq)).arg(nearestDB, 0, 'f', 1);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 200));
        p.drawRoundedRect(m_hoverPos.x() + 8, m_hoverPos.y() - 25, 140, 22, 4, 4);
        p.setPen(QColor("#ffffff"));
        p.drawText(m_hoverPos.x() + 14, m_hoverPos.y() - 10, tooltip);
    }
}
