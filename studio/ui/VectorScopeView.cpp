#include "ui/VectorScopeView.h"

#include <QEvent>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

VectorScopeView::VectorScopeView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

VectorScopeView::VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setEngine(engine);
}

VectorScopeView::~VectorScopeView() {
    if (isVisible() && m_engine && m_engine->visibilityCount > 0) {
        m_engine->visibilityCount--;
    }
}

void VectorScopeView::setEngine(std::shared_ptr<VectorScopeEngine> engine) {
    if (m_engine) {
        if (isVisible() && m_engine->visibilityCount > 0)
            m_engine->visibilityCount--;
        disconnect(m_engine.get(), &VectorScopeEngine::updated, this, nullptr);
    }
    m_engine = engine;
    if (m_engine) {
        if (isVisible())
            m_engine->visibilityCount++;
        connect(m_engine.get(), &VectorScopeEngine::updated, this, [this]() {
            if (m_engine)
                setSamples(m_engine->samples, m_engine->showParticles, m_engine->autoScale, m_engine->channelL,
                           m_engine->channelR, m_engine->traceDecayRate);
        });
        setSamples(m_engine->samples, m_engine->showParticles, m_engine->autoScale, m_engine->channelL,
                   m_engine->channelR, m_engine->traceDecayRate);
    }
}

void VectorScopeView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_engine)
        m_engine->visibilityCount++;
}

void VectorScopeView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0)
        m_engine->visibilityCount--;
}

void VectorScopeView::setSamples(const AudioSamplesData& samples, bool showParticles, bool autoScale, int channelL,
                                 int channelR, float traceDecayRate) {
    m_samples = samples;
    m_showParticles = showParticles;
    m_autoScale = autoScale;
    m_channelL = channelL;
    m_channelR = channelR;
    m_traceDecayRate = traceDecayRate;
    if (m_samples.channels.empty() && m_samples.left().empty() && m_samples.right().empty()) {
        m_persistenceBuffer = QImage();
        m_autoScaleFactor = 1.0f;
    }
    update();
}

void VectorScopeView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        m_persistenceBuffer = QImage();
        update();
    }
}

void VectorScopeView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    if (w < 20 || h < 20)
        return;

    bool inMiniPlayer = (parentWidget() && parentWidget()->inherits("QStackedWidget"));
    if (!inMiniPlayer) {
        p.fillRect(rect(), palette().color(QPalette::Base));
    }

    int topMargin = 28;
    int bottomMargin = 32;
    int drawH = h - topMargin - bottomMargin;
    int centerRadius = std::max(10, std::min(w - 30, drawH) / 2);
    QPoint centerPt(w / 2, topMargin + drawH / 2);

    // 1. Draw Reticle axes (+M, -M, +S, -S, L, R) crisply on main painter p
    QColor mainAxisCol = palette().color(QPalette::Mid);
    QColor diagAxisCol = palette().color(QPalette::Mid);
    QColor labelCol = palette().color(QPalette::PlaceholderText);

    p.setPen(QPen(mainAxisCol, 1, Qt::SolidLine));
    p.drawLine(centerPt.x() - centerRadius, centerPt.y(), centerPt.x() + centerRadius, centerPt.y());
    p.drawLine(centerPt.x(), centerPt.y() - centerRadius, centerPt.x(), centerPt.y() + centerRadius);

    int offset = static_cast<int>(centerRadius * 0.7071);
    p.setPen(QPen(diagAxisCol, 0.5, Qt::SolidLine));
    p.drawLine(centerPt.x() - offset, centerPt.y() - offset, centerPt.x() + offset, centerPt.y() + offset);
    p.drawLine(centerPt.x() - offset, centerPt.y() + offset, centerPt.x() + offset, centerPt.y() - offset);
    p.drawEllipse(centerPt, centerRadius * 3 / 4, centerRadius * 3 / 4);

    // Scope polar axis labels
    QFont axisFont = font();
    axisFont.setPointSize(7);
    axisFont.setBold(true);
    p.setFont(axisFont);
    p.setPen(labelCol);

    p.drawText(QRect(centerPt.x() - 15, centerPt.y() - centerRadius - 14, 30, 12), Qt::AlignCenter, "+M");
    p.drawText(QRect(centerPt.x() - 15, centerPt.y() + centerRadius + 2, 30, 12), Qt::AlignCenter, "-M");
    p.drawText(QRect(centerPt.x() - centerRadius - 20, centerPt.y() - 6, 18, 12), Qt::AlignCenter, "-S");
    p.drawText(QRect(centerPt.x() + centerRadius + 2, centerPt.y() - 6, 18, 12), Qt::AlignCenter, "+S");

    p.drawText(QRect(centerPt.x() - offset - 14, centerPt.y() - offset - 12, 14, 12), Qt::AlignCenter, "L");
    p.drawText(QRect(centerPt.x() + offset, centerPt.y() - offset - 12, 14, 12), Qt::AlignCenter, "R");

    // 2. Allocate / resize offscreen phosphor persistence buffer if needed
    if (m_persistenceBuffer.size() != size()) {
        m_persistenceBuffer = QImage(size(), QImage::Format_ARGB32_Premultiplied);
        m_persistenceBuffer.fill(Qt::transparent);
    }

    // 3. Phosphor decay: fade existing trace content cleanly using DestinationIn
    {
        QPainter bufPainter(&m_persistenceBuffer);
        bufPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        int decayAlpha = static_cast<int>(std::max(0.0f, std::min(255.0f, 255.0f * m_traceDecayRate)));
        QRect decayRect = QRect(centerPt.x() - centerRadius - 8, centerPt.y() - centerRadius - 8,
                                (centerRadius + 8) * 2, (centerRadius + 8) * 2)
                              .intersected(m_persistenceBuffer.rect());
        bufPainter.fillRect(decayRect, QColor(0, 0, 0, decayAlpha));
        bufPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        bufPainter.setRenderHint(QPainter::Antialiasing);

        const auto& left = (m_channelL >= 0 && static_cast<size_t>(m_channelL) < m_samples.channels.size())
                               ? m_samples.channels[m_channelL]
                               : m_samples.left();
        const auto& right = (m_channelR >= 0 && static_cast<size_t>(m_channelR) < m_samples.channels.size())
                                ? m_samples.channels[m_channelR]
                                : m_samples.right();
        size_t count = std::min(left.size(), right.size());

        if (count > 0) {
            // Compute Phase Correlation and Stereo Balance with NaN sanitization
            double sumLR = 0.0, sumL2 = 0.0, sumR2 = 0.0;
            float maxVal = 0.0f;

            for (size_t i = 0; i < count; ++i) {
                float l = left[i];
                float r = right[i];
                if (std::isnan(l))
                    l = 0.0f;
                if (std::isnan(r))
                    r = 0.0f;

                sumLR += static_cast<double>(l) * r;
                sumL2 += static_cast<double>(l) * l;
                sumR2 += static_cast<double>(r) * r;

                float m = (l + r) * 0.7071f;
                float s = (l - r) * 0.7071f;
                maxVal = std::max(maxVal, std::max(std::abs(m), std::abs(s)));
            }

            float rawCorr =
                (sumL2 > 1e-9 && sumR2 > 1e-9) ? static_cast<float>(sumLR / (std::sqrt(sumL2 * sumR2))) : 1.0f;
            if (std::isnan(rawCorr))
                rawCorr = 1.0f;
            rawCorr = std::max(-1.0f, std::min(1.0f, rawCorr));
            if (std::isnan(m_phaseCorrSmoothed))
                m_phaseCorrSmoothed = rawCorr;
            else
                m_phaseCorrSmoothed = m_phaseCorrSmoothed * 0.85f + rawCorr * 0.15f;

            float rmsL = std::sqrt(static_cast<float>(sumL2 / count));
            float rmsR = std::sqrt(static_cast<float>(sumR2 / count));
            float rawBal = (rmsL + rmsR > 1e-6f) ? (rmsR - rmsL) / (rmsL + rmsR) : 0.0f;
            if (std::isnan(rawBal))
                rawBal = 0.0f;
            rawBal = std::max(-1.0f, std::min(1.0f, rawBal));
            if (std::isnan(m_balanceSmoothed))
                m_balanceSmoothed = rawBal;
            else
                m_balanceSmoothed = m_balanceSmoothed * 0.85f + rawBal * 0.15f;

            bool enableAutoScale = m_engine ? m_engine->autoScale : m_autoScale;
            float targetAutoScaleFactor = 1.0f;
            if (enableAutoScale && maxVal > 1e-6f && std::isfinite(maxVal)) {
                targetAutoScaleFactor = std::min(0.90f / maxVal, 32.0f);
            }
            if (std::isnan(m_autoScaleFactor))
                m_autoScaleFactor = targetAutoScaleFactor;
            else
                m_autoScaleFactor = m_autoScaleFactor * 0.95f + targetAutoScaleFactor * 0.05f;

            float scale = centerRadius * m_autoScaleFactor;

            if (!m_showParticles) {
                // Optimized Line Mode: Fast polyline drawing of all points
                std::vector<QPointF> pts;
                pts.reserve(count);

                for (size_t i = 0; i < count; ++i) {
                    float l = left[i];
                    float r = right[i];
                    if (std::isnan(l))
                        l = 0.0f;
                    if (std::isnan(r))
                        r = 0.0f;

                    float m = (l + r) * 0.7071f;
                    float s = (l - r) * 0.7071f;

                    float px = centerPt.x() + s * scale;
                    float py = centerPt.y() - m * scale;
                    pts.emplace_back(px, py);
                }

                if (pts.size() > 1) {
                    double lineWidth = std::max(1.0, static_cast<double>(centerRadius) / 100.0);
                    bufPainter.setPen(
                        QPen(QColor(0, 122, 255, 180), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    bufPainter.drawPolyline(pts.data(), static_cast<int>(pts.size()));
                }
            } else {
                // Optimized Particle Mode: Pre-computed 32-bin palette & batched draw of all points
                constexpr int NUM_BINS = 32;
                static const auto binColors = []() {
                    std::array<QColor, NUM_BINS> cols;
                    for (int b = 0; b < NUM_BINS; ++b) {
                        float t = static_cast<float>(b) / static_cast<float>(NUM_BINS - 1);
                        float alpha = 0.05f + 0.80f * t;
                        float hue = 0.65f - 0.15f * t;
                        cols[b] = QColor::fromHsvF(hue, 0.85f, 0.95f, alpha);
                    }
                    return cols;
                }();

                std::array<std::vector<QPointF>, NUM_BINS> binPoints;
                std::vector<QPointF> glowPoints;

                for (size_t i = 0; i < count; ++i) {
                    float l = left[i];
                    float r = right[i];
                    if (std::isnan(l))
                        l = 0.0f;
                    if (std::isnan(r))
                        r = 0.0f;

                    float m = (l + r) * 0.7071f;
                    float s = (l - r) * 0.7071f;

                    float px = centerPt.x() + s * scale;
                    float py = centerPt.y() - m * scale;

                    float t = (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : 1.0f;
                    int binIdx = std::clamp(static_cast<int>(t * NUM_BINS), 0, NUM_BINS - 1);
                    binPoints[binIdx].emplace_back(px, py);

                    if (t > 0.9f) {
                        glowPoints.emplace_back(px, py);
                    }
                }

                bufPainter.setPen(Qt::NoPen);

                // Draw main particles by bin
                for (int b = 0; b < NUM_BINS; ++b) {
                    if (binPoints[b].empty())
                        continue;
                    float t = static_cast<float>(b) / static_cast<float>(NUM_BINS - 1);
                    float particleSize = 1.2f + 3.0f * t;
                    float halfSize = particleSize / 2.0f;

                    bufPainter.setBrush(binColors[b]);
                    for (const auto& pt : binPoints[b]) {
                        bufPainter.drawEllipse(pt, halfSize, halfSize);
                    }
                }

                // Draw glow halos for head of stream
                if (!glowPoints.empty()) {
                    bufPainter.setBrush(QColor(binColors[NUM_BINS - 1].red(), binColors[NUM_BINS - 1].green(),
                                               binColors[NUM_BINS - 1].blue(), 60));
                    for (const auto& pt : glowPoints) {
                        bufPainter.drawEllipse(pt, 4.0f, 4.0f);
                    }
                }
            }
        }
    }

    // 4. Draw offscreen buffer with phosphor persistence decay
    p.drawImage(0, 0, m_persistenceBuffer);

    // 5. Draw Dynamic Balance Indicator bar (Top)
    {
        int barW = std::min(w - 80, 240);
        int barX = (w - barW) / 2;
        int barY = 10;
        int barH = 6;

        QFont labelF = font();
        labelF.setPointSize(8);
        labelF.setBold(true);
        p.setFont(labelF);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(barX - 16, barY + 7, "L");
        p.drawText(barX + barW + 6, barY + 7, "R");

        QColor barBg = palette().color(QPalette::Mid);
        p.setBrush(barBg);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(barX, barY, barW, barH, 3, 3);

        // Center tick mark
        p.setPen(QPen(palette().color(QPalette::PlaceholderText), 1));
        p.drawLine(barX + barW / 2, barY - 2, barX + barW / 2, barY + barH + 2);

        // Dynamic Balance Marker
        float bal = m_balanceSmoothed;
        if (std::isnan(bal))
            bal = 0.0f;
        float balPos = barX + barW / 2.0f + bal * (barW / 2.0f - 4);
        p.setBrush(QColor("#00c6ff"));
        p.setPen(QPen(QColor("#ffffff"), 1));
        p.drawEllipse(QPointF(balPos, barY + barH / 2.0f), 4, 4);
    }

    // 6. Draw Phase Correlation Meter Bar (-1 to +1) (Bottom)
    {
        int barW = std::max(60, std::min(w - 110, 240));
        int barX = (w - barW) / 2 - 12;
        int barY = h - 22;
        int barH = 8;

        QFont labelF = font();
        labelF.setPointSize(8);
        labelF.setBold(true);
        p.setFont(labelF);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(barX - 22, barY + 8, "-1");
        p.drawText(barX + barW / 2 - 4, barY + 8, "0");
        p.drawText(barX + barW + 4, barY + 8, "+1");

        QColor barBg = palette().color(QPalette::Mid);
        p.setBrush(barBg);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(barX, barY, barW, barH, 4, 4);

        // Center line (0 correlation)
        int centerX = barX + barW / 2;
        p.setPen(QPen(palette().color(QPalette::PlaceholderText), 1));
        p.drawLine(centerX, barY - 2, centerX, barY + barH + 2);

        // Indicator Bar fill from Center (0) to m_phaseCorrSmoothed
        float corrVal = m_phaseCorrSmoothed;
        if (std::isnan(corrVal))
            corrVal = 0.0f;
        int fillW = static_cast<int>(corrVal * (barW / 2.0f));

        QColor corrColor;
        if (corrVal > 0.3f)
            corrColor = QColor(52, 199, 89); // Green (Mono in-phase)
        else if (corrVal >= -0.3f)
            corrColor = QColor(255, 204, 0); // Yellow (Decorrelated)
        else
            corrColor = QColor(255, 59, 48); // Red (Out of phase)

        p.setBrush(corrColor);
        p.setPen(Qt::NoPen);
        if (fillW >= 0) {
            p.drawRoundedRect(centerX, barY, fillW, barH, 2, 2);
        } else {
            p.drawRoundedRect(centerX + fillW, barY, -fillW, barH, 2, 2);
        }

        // Numeric label
        p.setPen(corrColor);
        p.drawText(barX + barW + 24, barY + 8, QString("%1%2").arg(corrVal >= 0 ? "+" : "").arg(corrVal, 0, 'f', 2));
    }
}
