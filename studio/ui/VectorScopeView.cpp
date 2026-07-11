#include "ui/VectorScopeView.h"

#include "ui/StyleTheme.h"

#include <QEvent>
#include <QPainterPath>
#include <cmath>

VectorScopeView::VectorScopeView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

VectorScopeView::VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setEngine(engine);
}

void VectorScopeView::setEngine(std::shared_ptr<VectorScopeEngine> engine) {
    if (m_engine) {
        disconnect(m_engine.get(), &VectorScopeEngine::updated, this, nullptr);
    }
    m_engine = engine;
    if (m_engine) {
        connect(m_engine.get(), &VectorScopeEngine::updated, this, [this]() {
            if (m_engine)
                setSamples(m_engine->samples, m_engine->showParticles, m_engine->autoScale);
        });
        setSamples(m_engine->samples, m_engine->showParticles, m_engine->autoScale);
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

void VectorScopeView::setSamples(const AudioSamplesData& samples, bool showParticles, bool autoScale) {
    m_samples = samples;
    m_showParticles = showParticles;
    m_autoScale = autoScale;
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

    // Allocate / resize offscreen phosphor persistence buffer if needed
    if (m_persistenceBuffer.size() != size()) {
        m_persistenceBuffer = QImage(size(), QImage::Format_ARGB32_Premultiplied);
        m_persistenceBuffer.fill(StyleTheme::cardBg());
    }

    // 1. Phosphor decay: fade existing buffer content slightly toward background
    {
        QPainter bufPainter(&m_persistenceBuffer);
        bufPainter.setRenderHint(QPainter::Antialiasing);
        QColor fadeColor = StyleTheme::cardBg();
        fadeColor.setAlpha(45); // Alpha buffer decay rate (~82% retention)
        bufPainter.fillRect(m_persistenceBuffer.rect(), fadeColor);

        // Reticle axes (+M, -M, +S, -S) & Diagonal Corner-to-Corner X grid lines
        int topMargin = 28;
        int bottomMargin = 32;
        int drawH = h - topMargin - bottomMargin;
        int centerRadius = std::min(w - 20, drawH) / 2;
        QPoint centerPt(w / 2, topMargin + drawH / 2);

        QColor mainAxisCol = StyleTheme::isDark() ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 25);
        QColor diagAxisCol = StyleTheme::isDark() ? QColor(255, 255, 255, 13) : QColor(0, 0, 0, 13);

        bufPainter.setPen(QPen(mainAxisCol, 1, Qt::SolidLine));
        bufPainter.drawLine(centerPt.x() - centerRadius, centerPt.y(), centerPt.x() + centerRadius, centerPt.y());
        bufPainter.drawLine(centerPt.x(), centerPt.y() - centerRadius, centerPt.x(), centerPt.y() + centerRadius);

        int offset = static_cast<int>(centerRadius * 0.7071);
        bufPainter.setPen(QPen(diagAxisCol, 0.5, Qt::SolidLine));
        bufPainter.drawLine(centerPt.x() - offset, centerPt.y() - offset, centerPt.x() + offset, centerPt.y() + offset);
        bufPainter.drawLine(centerPt.x() - offset, centerPt.y() + offset, centerPt.x() + offset, centerPt.y() - offset);
        bufPainter.drawEllipse(centerPt, centerRadius * 3 / 4, centerRadius * 3 / 4);

        const auto& left = m_samples.left();
        const auto& right = m_samples.right();
        size_t count = std::min(left.size(), right.size());

        if (count > 0) {
            // Compute Phase Correlation and Stereo Balance
            double sumLR = 0.0, sumL2 = 0.0, sumR2 = 0.0;
            float maxVal = 0.0f;

            for (size_t i = 0; i < count; ++i) {
                float l = left[i];
                float r = right[i];
                sumLR += static_cast<double>(l) * r;
                sumL2 += static_cast<double>(l) * l;
                sumR2 += static_cast<double>(r) * r;

                float m = (l + r) * 0.7071f;
                float s = (l - r) * 0.7071f;
                maxVal = std::max(maxVal, std::max(std::abs(m), std::abs(s)));
            }

            float rawCorr =
                (sumL2 > 1e-9 && sumR2 > 1e-9) ? static_cast<float>(sumLR / (std::sqrt(sumL2 * sumR2))) : 1.0f;
            rawCorr = std::max(-1.0f, std::min(1.0f, rawCorr));
            m_phaseCorrSmoothed = m_phaseCorrSmoothed * 0.85f + rawCorr * 0.15f;

            float rmsL = std::sqrt(static_cast<float>(sumL2 / count));
            float rmsR = std::sqrt(static_cast<float>(sumR2 / count));
            float rawBal = (rmsL + rmsR > 1e-6f) ? (rmsR - rmsL) / (rmsL + rmsR) : 0.0f;
            rawBal = std::max(-1.0f, std::min(1.0f, rawBal));
            m_balanceSmoothed = m_balanceSmoothed * 0.85f + rawBal * 0.15f;

            bool enableAutoScale = m_engine ? m_engine->autoScale : m_autoScale;
            float autoScaleFactor = 1.0f;
            if (enableAutoScale && maxVal > 1e-6f && std::isfinite(maxVal)) {
                autoScaleFactor = std::min(0.90f / maxVal, 32.0f);
            }

            if (!m_showParticles) {
                QPainterPath path;
                for (size_t i = 0; i < count; ++i) {
                    float l = left[i];
                    float r = right[i];
                    float m = (l + r) * 0.7071f;
                    float s = (l - r) * 0.7071f;

                    float px = centerPt.x() + s * (centerRadius * autoScaleFactor);
                    float py = centerPt.y() - m * (centerRadius * autoScaleFactor);

                    if (i == 0)
                        path.moveTo(px, py);
                    else
                        path.lineTo(px, py);
                }
                double lineWidth = std::max(1.0, static_cast<double>(centerRadius) / 75.0);
                bufPainter.setPen(QPen(QColor(0, 122, 255, 178), lineWidth));
                bufPainter.drawPath(path);
            } else {
                bufPainter.setPen(Qt::NoPen);
                for (size_t i = 0; i < count; ++i) {
                    float l = left[i];
                    float r = right[i];
                    float t = (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : 1.0f;

                    float m = (l + r) * 0.7071f;
                    float s = (l - r) * 0.7071f;

                    float px = centerPt.x() + s * (centerRadius * autoScaleFactor);
                    float py = centerPt.y() - m * (centerRadius * autoScaleFactor);

                    float particleSize = 1.0f + 3.5f * t;
                    float alpha = 0.05f + 0.85f * t;

                    float hue = 0.65f - 0.15f * t;
                    QColor particleColor = QColor::fromHsvF(hue, 0.85f, 0.95f, alpha);

                    if (t > 0.9f) {
                        float glowSize = particleSize * 2.2f;
                        QColor haloColor = particleColor;
                        haloColor.setAlphaF(0.30f);
                        bufPainter.setBrush(haloColor);
                        bufPainter.drawEllipse(QPointF(px, py), glowSize / 2.0f, glowSize / 2.0f);
                    }

                    bufPainter.setBrush(particleColor);
                    bufPainter.drawEllipse(QPointF(px, py), particleSize / 2.0f, particleSize / 2.0f);
                }
            }
        }
    }

    // 2. Draw offscreen buffer with phosphor persistence decay
    p.drawImage(0, 0, m_persistenceBuffer);

    // 3. Draw Dynamic Balance Indicator bar (Top)
    {
        int barW = std::min(w - 80, 240);
        int barX = (w - barW) / 2;
        int barY = 10;
        int barH = 6;

        p.setFont(QFont("sans-serif", 8, QFont::Bold));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(barX - 16, barY + 7, "L");
        p.drawText(barX + barW + 6, barY + 7, "R");

        p.setBrush(StyleTheme::isDark() ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 25));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(barX, barY, barW, barH, 3, 3);

        // Center tick mark
        p.setPen(QPen(StyleTheme::axisLabelPenColor(), 1));
        p.drawLine(barX + barW / 2, barY - 2, barX + barW / 2, barY + barH + 2);

        // Dynamic Balance Marker
        float balPos = barX + barW / 2.0f + m_balanceSmoothed * (barW / 2.0f - 4);
        p.setBrush(QColor("#00c6ff"));
        p.setPen(QPen(QColor("#ffffff"), 1));
        p.drawEllipse(QPointF(balPos, barY + barH / 2.0f), 4, 4);
    }

    // 4. Draw Phase Correlation Meter Bar (-1 to +1) (Bottom)
    {
        int barW = std::min(w - 80, 260);
        int barX = (w - barW) / 2;
        int barY = h - 22;
        int barH = 8;

        p.setFont(QFont("sans-serif", 8, QFont::Bold));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(barX - 22, barY + 8, "-1");
        p.drawText(barX + barW / 2 - 4, barY + 8, "0");
        p.drawText(barX + barW + 4, barY + 8, "+1");

        p.setBrush(StyleTheme::isDark() ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 30));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(barX, barY, barW, barH, 4, 4);

        // Center line (0 correlation)
        int centerX = barX + barW / 2;
        p.setPen(QPen(StyleTheme::axisLabelPenColor(), 1));
        p.drawLine(centerX, barY - 2, centerX, barY + barH + 2);

        // Indicator Bar fill from Center (0) to m_phaseCorrSmoothed
        float corrVal = m_phaseCorrSmoothed;
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
        p.drawText(barX + barW + 20, barY + 8, QString("%1%2").arg(corrVal >= 0 ? "+" : "").arg(corrVal, 0, 'f', 2));
    }
}
