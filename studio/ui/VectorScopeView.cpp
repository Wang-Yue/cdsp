#include "ui/VectorScopeView.h"

#include "utils/ThemeManager.h" // for ThemeManager

#include <QBrush>      // for QBrush
#include <QColor>      // for QColor
#include <QEvent>      // for QEvent
#include <QFont>       // for QFont
#include <QLine>       // for QLine
#include <QPainter>    // for QPainter
#include <QPalette>    // for QPalette
#include <QPen>        // for QPen
#include <QPoint>      // for QPoint
#include <QRect>       // for QRect
#include <QSizePolicy> // for QSizePolicy
#include <Qt>          // for AlignmentFlag, PenStyle, PenCapStyle
#include <QtGlobal>    // for Q_UNUSED
#include <algorithm>   // for max, min, clamp
#include <cmath>       // for isnan, isfinite, sqrt
#include <stddef.h>    // for size_t
#include <vector>      // for vector

VectorScopeView::VectorScopeView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(40, 24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VectorScopeView::VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent) : QWidget(parent) {
    setMinimumSize(40, 24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
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
    if (!isVisible())
        return;
    m_samples = samples;
    m_showParticles = showParticles;
    m_autoScale = autoScale;
    m_channelL = channelL;
    m_channelR = channelR;
    m_traceDecayRate = std::clamp(traceDecayRate, 0.0f, 0.999f);

    if (m_samples.channels.empty() && m_samples.left().empty() && m_samples.right().empty()) {
        m_autoScaleFactor = 1.0f;
    }
    update();
}

void VectorScopeView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        update();
    }
}

void VectorScopeView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    int w = width();
    int h = height();
    if (w < 20 || h < 20)
        return;

    bool inMiniPlayer = ThemeManager::isMiniPlayer(this);
    if (!inMiniPlayer) {
        p.fillRect(rect(), palette().color(QPalette::Window));
    }

    int margin = inMiniPlayer ? 4 : 16;
    int drawW = w - 2 * margin;
    int drawH = h - 2 * margin;
    int scaleX = std::max(6, drawW / 2);
    int scaleY = std::max(6, drawH / 2);
    QPoint centerPt(w / 2, h / 2);

    // 1. Draw Reticle axes (+M, -M, +S, -S, L, R)
    QColor mainAxisCol = inMiniPlayer ? QColor(255, 255, 255, 35) : palette().color(QPalette::Mid);
    QColor diagAxisCol = ThemeManager::gridColor(this);
    QColor labelCol = ThemeManager::subtextColor(this);

    p.setPen(QPen(mainAxisCol, 1, Qt::SolidLine));
    p.drawLine(centerPt.x() - scaleX, centerPt.y(), centerPt.x() + scaleX, centerPt.y());
    p.drawLine(centerPt.x(), centerPt.y() - scaleY, centerPt.x(), centerPt.y() + scaleY);

    int offsetX = static_cast<int>(scaleX * 0.7071);
    int offsetY = static_cast<int>(scaleY * 0.7071);
    p.setPen(QPen(diagAxisCol, 0.5, Qt::SolidLine));
    p.drawLine(centerPt.x() - offsetX, centerPt.y() - offsetY, centerPt.x() + offsetX, centerPt.y() + offsetY);
    p.drawLine(centerPt.x() - offsetX, centerPt.y() + offsetY, centerPt.x() + offsetX, centerPt.y() - offsetY);
    p.drawEllipse(centerPt, scaleX * 3 / 4, scaleY * 3 / 4);

    // Scope polar axis labels (only when enough vertical & horizontal space is available)
    if (scaleY >= 28 && scaleX >= 28) {
        QFont axisFont = font();
        axisFont.setPointSize(7);
        axisFont.setBold(true);
        p.setFont(axisFont);
        p.setPen(labelCol);

        p.drawText(QRect(centerPt.x() - 15, centerPt.y() - scaleY - 14, 30, 12), Qt::AlignCenter, "+M");
        p.drawText(QRect(centerPt.x() - 15, centerPt.y() + scaleY + 2, 30, 12), Qt::AlignCenter, "-M");
        p.drawText(QRect(centerPt.x() - scaleX - 20, centerPt.y() - 6, 18, 12), Qt::AlignCenter, "-S");
        p.drawText(QRect(centerPt.x() + scaleX + 2, centerPt.y() - 6, 18, 12), Qt::AlignCenter, "+S");

        p.drawText(QRect(centerPt.x() - offsetX - 14, centerPt.y() - offsetY - 12, 14, 12), Qt::AlignCenter, "L");
        p.drawText(QRect(centerPt.x() + offsetX, centerPt.y() - offsetY - 12, 14, 12), Qt::AlignCenter, "R");
    }

    // 2. Direct GPU Audio Trace Drawing
    int chL = m_engine ? m_engine->channelL : m_channelL;
    int chR = m_engine ? m_engine->channelR : m_channelR;
    bool showParticles = m_engine ? m_engine->showParticles : m_showParticles;
    bool enableAutoScale = m_engine ? m_engine->autoScale : m_autoScale;

    const auto& left =
        (chL >= 0 && static_cast<size_t>(chL) < m_samples.channels.size()) ? m_samples.channels[chL] : m_samples.left();
    const auto& right = (chR >= 0 && static_cast<size_t>(chR) < m_samples.channels.size()) ? m_samples.channels[chR]
                                                                                           : m_samples.right();
    size_t count = std::min(left.size(), right.size());

    if (count > 0) {
        // Standard Mid/Side rotation:
        // m = (L + R) / 2
        // s = (L - R) / 2
        float maxNormSq = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            float l = left[i];
            float r = right[i];
            if (std::isnan(l))
                l = 0.0f;
            if (std::isnan(r))
                r = 0.0f;

            float m = (l + r) * 0.5f;
            float s = (l - r) * 0.5f;
            float normSq = m * m + s * s;
            if (normSq > maxNormSq)
                maxNormSq = normSq;
        }
        float maxVectorNorm = std::sqrt(maxNormSq);

        if (enableAutoScale) {
            float targetAutoScaleFactor = 1.0f;
            // Only auto-scale signals above noise floor (-60 dBFS / 0.001)
            if (maxVectorNorm > 0.001f && std::isfinite(maxVectorNorm)) {
                targetAutoScaleFactor = std::clamp(0.90f / maxVectorNorm, 1.0f, 16.0f);
            }
            if (std::isnan(m_autoScaleFactor))
                m_autoScaleFactor = targetAutoScaleFactor;
            else
                m_autoScaleFactor = m_autoScaleFactor * 0.90f + targetAutoScaleFactor * 0.10f;
        } else {
            // Immediate reset when auto-scale is disabled
            m_autoScaleFactor = 1.0f;
        }

        float activeScaleX = scaleX * m_autoScaleFactor;
        float activeScaleY = scaleY * m_autoScaleFactor;

        if (!showParticles) {
            // Disjoint drawLines with pixel-bucketing / downsampling
            m_cachedLines.clear();
            m_cachedLines.reserve(count);

            int prevX = -999999;
            int prevY = -999999;
            bool hasPrev = false;

            for (size_t i = 0; i < count; ++i) {
                float l = left[i];
                float r = right[i];
                if (std::isnan(l))
                    l = 0.0f;
                if (std::isnan(r))
                    r = 0.0f;

                float m = (l + r) * 0.5f;
                float s = (l - r) * 0.5f;

                int px = static_cast<int>(centerPt.x() + s * activeScaleX);
                int py = static_cast<int>(centerPt.y() - m * activeScaleY);

                if (!hasPrev) {
                    prevX = px;
                    prevY = py;
                    hasPrev = true;
                    continue;
                }

                // Skip zero-length or sub-pixel micro jitter
                if (px == prevX && py == prevY)
                    continue;

                m_cachedLines.emplace_back(prevX, prevY, px, py);
                prevX = px;
                prevY = py;
            }

            if (!m_cachedLines.empty()) {
                double lineWidth = std::max(1.0, std::min(scaleX, scaleY) / 100.0);
                p.setPen(QPen(QColor(0, 160, 255, 210), lineWidth, Qt::SolidLine, Qt::FlatCap));
                p.drawLines(m_cachedLines.data(), static_cast<int>(m_cachedLines.size()));
            }
        } else {
            // Particle Mode with pixel deduplication
            m_cachedPoints.clear();
            m_cachedPoints.reserve(count);

            int lastX = -999999;
            int lastY = -999999;

            for (size_t i = 0; i < count; ++i) {
                float l = left[i];
                float r = right[i];
                if (std::isnan(l))
                    l = 0.0f;
                if (std::isnan(r))
                    r = 0.0f;

                float m = (l + r) * 0.5f;
                float s = (l - r) * 0.5f;

                int px = static_cast<int>(centerPt.x() + s * activeScaleX);
                int py = static_cast<int>(centerPt.y() - m * activeScaleY);

                if (px == lastX && py == lastY)
                    continue;

                lastX = px;
                lastY = py;
                m_cachedPoints.emplace_back(px, py);
            }

            if (!m_cachedPoints.empty()) {
                double baseSize = std::max(2.0, std::min(scaleX, scaleY) / 80.0);

                // Pass 1: Base particle cloud
                p.setPen(QPen(QColor(0, 180, 255, 160), baseSize, Qt::SolidLine, Qt::SquareCap));
                p.drawPoints(m_cachedPoints.data(), static_cast<int>(m_cachedPoints.size()));

                // Pass 2: Head particle burst (latest 15% of samples)
                size_t headStart = static_cast<size_t>(m_cachedPoints.size() * 0.85);
                if (headStart < m_cachedPoints.size()) {
                    int headCount = static_cast<int>(m_cachedPoints.size() - headStart);
                    double headSize = baseSize * 1.5;
                    p.setPen(QPen(QColor(80, 255, 210, 230), headSize, Qt::SolidLine, Qt::SquareCap));
                    p.drawPoints(m_cachedPoints.data() + headStart, headCount);
                }
            }
        }
    } else {
        // When there are no samples or audio is stopped, decay back to 1.0f
        m_autoScaleFactor = 1.0f;
    }
}
