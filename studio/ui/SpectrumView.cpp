#include "ui/SpectrumView.h"

#include "ui/StyleTheme.h"

#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

SpectrumView::SpectrumView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setMouseTracking(true);
}

SpectrumView::SpectrumView(std::shared_ptr<SpectrumEngine> engine, QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setMouseTracking(true);
    setEngine(engine);
}

static std::vector<float> applyOctaveSmoothing(const std::vector<float>& freqs, const std::vector<float>& mags,
                                               OctaveSmoothing smoothing) {
    if (smoothing == OctaveSmoothing::None || freqs.size() != mags.size() || freqs.empty()) {
        return mags;
    }
    double octaveFraction = 1.0;
    switch (smoothing) {
    case OctaveSmoothing::OneThird:
        octaveFraction = 3.0;
        break;
    case OctaveSmoothing::OneSixth:
        octaveFraction = 6.0;
        break;
    case OctaveSmoothing::OneTwelfth:
        octaveFraction = 12.0;
        break;
    case OctaveSmoothing::OneTwentyFourth:
        octaveFraction = 24.0;
        break;
    default:
        return mags;
    }
    double factor = std::pow(2.0, 1.0 / (2.0 * octaveFraction));
    size_t count = freqs.size();
    std::vector<float> smoothed(count);

    for (size_t i = 0; i < count; ++i) {
        double centerF = freqs[i];
        double minF = centerF / factor;
        double maxF = centerF * factor;

        double sumWeight = 0.0;
        double sumVal = 0.0;

        for (size_t j = 0; j < count; ++j) {
            double f = freqs[j];
            if (f >= minF && f <= maxF) {
                double w = 1.0 - std::abs(std::log2(f / centerF)) * octaveFraction;
                w = std::max(0.001, w);
                sumVal += mags[j] * w;
                sumWeight += w;
            }
        }
        smoothed[i] = (sumWeight > 0.0) ? static_cast<float>(sumVal / sumWeight) : mags[i];
    }
    return smoothed;
}

void SpectrumView::setEngine(std::shared_ptr<SpectrumEngine> engine) {
    if (m_engine) {
        disconnect(m_engine.get(), &SpectrumEngine::updated, this, nullptr);
    }
    m_engine = engine;
    if (m_engine) {
        connect(m_engine.get(), &SpectrumEngine::updated, this, [this]() {
            if (m_engine)
                setSpectrum(m_engine->data, m_engine->smoothing, m_engine->peakHoldDecayRate);
        });
        setSpectrum(m_engine->data, m_engine->smoothing, m_engine->peakHoldDecayRate);
    }
}

void SpectrumView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_engine)
        m_engine->visibilityCount++;
}

void SpectrumView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0)
        m_engine->visibilityCount--;
}

static float normDB(float db, float minDB = -120.0f, float maxDB = 0.0f) {
    if (minDB >= maxDB)
        minDB = maxDB - 1.0f;
    if (db < minDB)
        return 0.0f;
    if (db > maxDB)
        return 1.0f;
    return (db - minDB) / (maxDB - minDB);
}

void SpectrumView::setSpectrum(const SpectrumData& data, OctaveSmoothing smoothing, float peakHoldDecayRate) {
    m_data = data;
    m_smoothing = smoothing;
    m_peakHoldDecayRate = peakHoldDecayRate;

    if (m_smoothing != OctaveSmoothing::None) {
        m_data.magnitudes = applyOctaveSmoothing(m_data.frequencies, m_data.magnitudes, m_smoothing);
    }

    float minDB = m_engine ? static_cast<float>(m_engine->minDB) : -120.0f;
    float maxDB = m_engine ? static_cast<float>(m_engine->maxDB) : 0.0f;

    if (m_peakHoldDecayRate <= 0.001f) {
        m_peakHold.clear();
    } else if (m_peakHold.size() != m_data.magnitudes.size()) {
        m_peakHold.resize(m_data.magnitudes.size(), 0.0f);
        for (size_t i = 0; i < m_data.magnitudes.size(); ++i) {
            m_peakHold[i] = normDB(m_data.magnitudes[i], minDB, maxDB);
        }
    } else {
        for (size_t i = 0; i < m_data.magnitudes.size(); ++i) {
            float normVal = normDB(m_data.magnitudes[i], minDB, maxDB);
            if (normVal >= m_peakHold[i]) {
                m_peakHold[i] = normVal;
            } else {
                m_peakHold[i] = std::max(0.0f, m_peakHold[i] * m_peakHoldDecayRate);
            }
        }
    }

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

    if (!parentWidget() || !parentWidget()->inherits("QStackedWidget")) {
        p.fillRect(rect(), StyleTheme::cardBg());
    }

    int w = width();
    int h = height();
    int marginL = 45;
    int marginB = 24;
    int marginT = 12;

    int plotW = w - marginL;
    int plotH = h - marginB - marginT;

    if (plotW < 20 || plotH < 20)
        return;

    float minDB = m_engine ? static_cast<float>(m_engine->minDB) : -120.0f;
    float maxDB = m_engine ? static_cast<float>(m_engine->maxDB) : 0.0f;

    // 1. dB Grid lines & labels
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(8);
    p.setFont(monoFont);

    QColor gridPenCol = StyleTheme::isDark() ? QColor(255, 255, 255, 13) : QColor(0, 0, 0, 13);

    double dbStep = (maxDB - minDB) > 60.0f ? 20.0 : 12.0;
    for (double db = maxDB; db >= minDB; db -= dbStep) {
        double normY = normDB(static_cast<float>(db), minDB, maxDB);
        double y = marginT + plotH * (1.0 - normY);

        p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
        p.drawLine(marginL, y, w, y);

        p.setPen(StyleTheme::textSecondary());
        p.drawText(QRectF(2, y - 6, marginL - 6, 12), Qt::AlignRight | Qt::AlignVCenter,
                   QString("%1").arg(static_cast<int>(db)));
    }

    // 2. Freq Grid lines & log-frequency ticks (20, 50, 100, 200, 500, 1k, 2k, 5k, 10k, 20k)
    double minF = (m_engine && m_engine->minFreq > 0) ? m_engine->minFreq : 25.0;
    double maxF = (m_engine && m_engine->maxFreq > 0) ? m_engine->maxFreq : 20000.0;
    double logMin = std::log10(minF), logMax = std::log10(maxF);

    QFont freqFont("sans-serif", 7);

    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (f < minF || f > maxF)
            continue;
        double fracX = (std::log10(f) - logMin) / (logMax - logMin);
        double x = marginL + fracX * plotW;

        // Grid line
        p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
        p.drawLine(x, marginT, x, marginT + plotH);

        // Tick mark
        p.setPen(QPen(StyleTheme::axisLabelPenColor(), 1));
        p.drawLine(x, marginT + plotH, x, marginT + plotH + 4);

        // Label
        p.setFont(freqFont);
        p.setPen(StyleTheme::textSecondary());
        QString label = f >= 1000.0 ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        p.drawText(QRectF(x - 15, marginT + plotH + 4, 30, 14), Qt::AlignCenter, label);
    }

    size_t count = std::min(m_data.frequencies.size(), m_data.magnitudes.size());
    float spacing = 2.0f;

    // 3. Dynamic gradient audio level bars (green -> yellow -> orange -> red)
    QLinearGradient barGrad(0, marginT + plotH, 0, marginT);
    barGrad.setColorAt(0.0, QColor("#34c759"));
    barGrad.setColorAt(0.35, QColor("#34c759"));
    barGrad.setColorAt(0.55, QColor("#ffcc00"));
    barGrad.setColorAt(0.75, QColor("#ff9500"));
    barGrad.setColorAt(0.95, QColor("#ff3b30"));
    barGrad.setColorAt(1.0, QColor("#ff3b30"));

    QPainterPath curvePath;
    for (size_t i = 0; i < count; ++i) {
        float freq = m_data.frequencies[i];
        if (freq < minF || freq > maxF)
            continue;

        double fracX = (std::log10(freq) - logMin) / (logMax - logMin);
        double x = marginL + fracX * plotW;

        // Dynamic bar width
        double xPrev =
            (i > 0) ? marginL + (std::log10(m_data.frequencies[i - 1]) - logMin) / (logMax - logMin) * plotW : x - 10.0;
        double xNext = (i + 1 < count)
                           ? marginL + (std::log10(m_data.frequencies[i + 1]) - logMin) / (logMax - logMin) * plotW
                           : x + 10.0;
        double barW = std::max(2.0, (xNext - xPrev) / 2.0 - spacing);

        float db = m_data.magnitudes[i];
        float normY = normDB(db, minDB, maxDB);
        double barHeight = std::max(2.0, static_cast<double>(normY * plotH));

        p.fillRect(QRectF(x - barW / 2.0, marginT + plotH - barHeight, barW, barHeight), barGrad);

        // Draw Peak Hold Line Segment
        if (m_peakHoldDecayRate > 0.001f && i < m_peakHold.size() && m_peakHold[i] > 0.001f) {
            float peakNormY = m_peakHold[i];
            double peakY = marginT + plotH - static_cast<double>(peakNormY * plotH);
            p.setPen(QPen(StyleTheme::isDark() ? QColor(255, 255, 255, 240) : QColor(30, 30, 30, 240), 1.8));
            p.drawLine(QPointF(x - barW / 2.0, peakY), QPointF(x + barW / 2.0, peakY));
        }

        if (i == 0)
            curvePath.moveTo(x, marginT + plotH - barHeight);
        else
            curvePath.lineTo(x, marginT + plotH - barHeight);
    }

    p.setPen(QPen(StyleTheme::gridPenColor(), 1.0));
    p.drawPath(curvePath);

    // 4. Hover Crosshair & Tooltip Readout Formatting
    if (m_isHovered && m_hoverPos.x() >= marginL && m_hoverPos.x() <= w && m_hoverPos.y() >= marginT &&
        m_hoverPos.y() <= marginT + plotH) {
        double mouseNormX = static_cast<double>(m_hoverPos.x() - marginL) / plotW;
        double targetFreq = std::pow(10.0, logMin + mouseNormX * (logMax - logMin));

        float nearestDB = -60.0f;
        double minDiff = 1e9;
        for (size_t i = 0; i < count; ++i) {
            double diff = std::abs(m_data.frequencies[i] - targetFreq);
            if (diff < minDiff) {
                minDiff = diff;
                nearestDB = m_data.magnitudes[i];
            }
        }

        // Draw vertical and horizontal crosshair lines
        p.setPen(QPen(QColor(255, 149, 0, 200), 1, Qt::DashLine));
        p.drawLine(m_hoverPos.x(), marginT, m_hoverPos.x(), marginT + plotH);
        p.drawLine(marginL, m_hoverPos.y(), w, m_hoverPos.y());

        // Hover intersection point dot
        p.setBrush(QColor("#ff9500"));
        p.setPen(QPen(QColor("#ffffff"), 1.5));
        p.drawEllipse(QPointF(m_hoverPos.x(), m_hoverPos.y()), 4, 4);

        // Tooltip formatting with formatted frequency and dB readout
        QString freqStr;
        if (targetFreq >= 1000.0) {
            freqStr = QString("%1 kHz").arg(targetFreq / 1000.0, 0, 'f', 2);
        } else {
            freqStr = QString("%1 Hz").arg(static_cast<int>(targetFreq));
        }
        QString tooltip = QString("%1  |  %2 dB").arg(freqStr).arg(nearestDB, 0, 'f', 1);

        int ttW = 150;
        int ttH = 24;
        int ttX = m_hoverPos.x() + 10;
        int ttY = m_hoverPos.y() - 30;

        if (ttX + ttW > w - 4)
            ttX = m_hoverPos.x() - ttW - 10;
        if (ttY < marginT + 4)
            ttY = m_hoverPos.y() + 10;

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(15, 20, 30, 230));
        p.drawRoundedRect(ttX, ttY, ttW, ttH, 5, 5);
        p.setPen(QPen(QColor("#ff9500"), 1));
        p.drawRoundedRect(ttX, ttY, ttW, ttH, 5, 5);

        p.setFont(QFont("sans-serif", 8, QFont::Bold));
        p.setPen(QColor("#ffffff"));
        p.drawText(QRectF(ttX, ttY, ttW, ttH), Qt::AlignCenter, tooltip);
    }
}
