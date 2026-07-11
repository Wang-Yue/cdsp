#include "ui/SpectrumView.h"
#include "ui/StyleTheme.h"
#include <QMouseEvent>
#include <QPainterPath>
#include <cmath>
#include <algorithm>

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

static float normDB60(float db) {
    if (db < -60.0f) return 0.0f;
    if (db > 0.0f) return 1.0f;
    return (db + 60.0f) / 60.0f;
}

void SpectrumView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    int w = width();
    int h = height();
    int marginB = 24;
    int plotH = h - marginB;

    // dB Grid lines & labels [-60, -48, -36, -24, -12, 0] dB
    p.setFont(QFont("sans-serif", 9));
    for (double db : {0, -12, -24, -36, -48, -60}) {
        double y = plotH - normDB60(static_cast<float>(db)) * plotH;
        p.setPen(QPen(QColor(255, 255, 255, 20), 1, Qt::DashLine));
        p.drawLine(0, y, w, y);
        p.setPen(QColor("#8e8e93"));
        p.drawText(4, y - 2, QString("%1 dB").arg(static_cast<int>(db)));
    }

    // Freq Grid lines & labels
    double minF = (m_engine && m_engine->minFreq > 0) ? m_engine->minFreq : 20.0;
    double maxF = (m_engine && m_engine->maxFreq > 0) ? m_engine->maxFreq : 20000.0;
    double logMin = std::log10(minF), logMax = std::log10(maxF);

    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (f < minF || f > maxF) continue;
        double x = (std::log10(f) - logMin) / (logMax - logMin) * w;
        p.setPen(QPen(QColor(255, 255, 255, 20), 1, Qt::DashLine));
        p.drawLine(x, 0, x, plotH);
        p.setPen(QColor("#8e8e93"));
        QString label = f >= 1000.0 ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        p.drawText(x - 10, h - 4, label);
    }

    if (m_data.frequencies.empty()) return;

    size_t count = m_data.frequencies.size();
    float spacing = 2.0f;
    float barW = std::max(2.0f, (w - spacing * static_cast<float>(count - 1)) / static_cast<float>(count));

    // Dynamic gradient audio level bars (green -> yellow -> orange -> red)
    QLinearGradient barGrad(0, plotH, 0, 0);
    barGrad.setColorAt(0.0, QColor("#34c759"));
    barGrad.setColorAt(0.35, QColor("#34c759"));
    barGrad.setColorAt(0.55, QColor("#ffcc00"));
    barGrad.setColorAt(0.75, QColor("#ff9500"));
    barGrad.setColorAt(0.95, QColor("#ff3b30"));
    barGrad.setColorAt(1.0, QColor("#ff3b30"));

    QPainterPath curvePath;
    for (size_t i = 0; i < count; ++i) {
        float freq = m_data.frequencies[i];
        if (freq < minF || freq > maxF) continue;

        float db = m_data.magnitudes[i];
        float normY = normDB60(db);
        double barHeight = std::max(2.0, static_cast<double>(normY * plotH));
        double x = (std::log10(freq) - logMin) / (logMax - logMin) * w;

        p.fillRect(QRectF(x - barW / 2.0, plotH - barHeight, barW, barHeight), barGrad);

        if (i == 0) curvePath.moveTo(x, plotH - barHeight);
        else curvePath.lineTo(x, plotH - barHeight);
    }

    p.setPen(QPen(QColor("#ffffff"), 1.5));
    p.drawPath(curvePath);

    // Hover readout
    if (m_isHovered && m_hoverPos.x() >= 0 && m_hoverPos.x() <= w) {
        double mouseNormX = static_cast<double>(m_hoverPos.x()) / w;
        double targetFreq = std::pow(10.0, logMin + mouseNormX * (logMax - logMin));

        p.setPen(QPen(QColor("#ff9500"), 1, Qt::SolidLine));
        p.drawLine(m_hoverPos.x(), 0, m_hoverPos.x(), plotH);

        float nearestDB = -60.0f;
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
        p.setBrush(QColor(0, 0, 0, 220));
        p.drawRoundedRect(m_hoverPos.x() + 8, m_hoverPos.y() - 25, 140, 22, 4, 4);
        p.setPen(QColor("#ffffff"));
        p.drawText(m_hoverPos.x() + 14, m_hoverPos.y() - 10, tooltip);
    }
}
