#include "ui/VectorScopeView.h"

#include <QEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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

    bool inMiniPlayer = (parentWidget() && parentWidget()->inherits("QStackedWidget"));
    if (!inMiniPlayer) {
        p.fillRect(rect(), palette().color(QPalette::Base));
    }

    int margin = inMiniPlayer ? 4 : 16;
    int drawW = w - 2 * margin;
    int drawH = h - 2 * margin;
    int centerRadius = std::max(6, std::min(drawW, drawH) / 2);
    QPoint centerPt(w / 2, h / 2);

    // 1. Draw Reticle axes (+M, -M, +S, -S, L, R)
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

    // Scope polar axis labels (only when enough space is available)
    if (centerRadius >= 28) {
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
    }

    // 2. Direct GPU Audio Trace Drawing
    const auto& left = (m_channelL >= 0 && static_cast<size_t>(m_channelL) < m_samples.channels.size())
                           ? m_samples.channels[m_channelL]
                           : m_samples.left();
    const auto& right = (m_channelR >= 0 && static_cast<size_t>(m_channelR) < m_samples.channels.size())
                            ? m_samples.channels[m_channelR]
                            : m_samples.right();
    size_t count = std::min(left.size(), right.size());

    if (count > 0) {
        float maxVal = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            float l = left[i];
            float r = right[i];
            if (std::isnan(l))
                l = 0.0f;
            if (std::isnan(r))
                r = 0.0f;

            float m = (l + r) * 0.7071f;
            float s = (l - r) * 0.7071f;
            maxVal = std::max(maxVal, std::max(std::abs(m), std::abs(s)));
        }

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
            // Option 2: Disjoint drawLines with pixel-bucketing / downsampling
            std::vector<QLine> lines;
            lines.reserve(count);

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

                float m = (l + r) * 0.7071f;
                float s = (l - r) * 0.7071f;

                int px = static_cast<int>(centerPt.x() + s * scale);
                int py = static_cast<int>(centerPt.y() - m * scale);

                if (!hasPrev) {
                    prevX = px;
                    prevY = py;
                    hasPrev = true;
                    continue;
                }

                // Skip zero-length or sub-pixel micro jitter
                if (px == prevX && py == prevY)
                    continue;

                lines.emplace_back(prevX, prevY, px, py);
                prevX = px;
                prevY = py;
            }

            if (!lines.empty()) {
                double lineWidth = std::max(1.0, static_cast<double>(centerRadius) / 100.0);
                p.setPen(QPen(QColor(0, 160, 255, 210), lineWidth, Qt::SolidLine, Qt::FlatCap));
                p.drawLines(lines.data(), static_cast<int>(lines.size()));
            }
        } else {
            // Particle Mode with pixel deduplication
            std::vector<QPoint> pts;
            pts.reserve(count);

            int lastX = -999999;
            int lastY = -999999;

            for (size_t i = 0; i < count; ++i) {
                float l = left[i];
                float r = right[i];
                if (std::isnan(l))
                    l = 0.0f;
                if (std::isnan(r))
                    r = 0.0f;

                float m = (l + r) * 0.7071f;
                float s = (l - r) * 0.7071f;

                int px = static_cast<int>(centerPt.x() + s * scale);
                int py = static_cast<int>(centerPt.y() - m * scale);

                if (px == lastX && py == lastY)
                    continue;

                lastX = px;
                lastY = py;
                pts.emplace_back(px, py);
            }

            if (!pts.empty()) {
                double baseSize = std::max(2.0, static_cast<double>(centerRadius) / 80.0);

                // Pass 1: Base particle cloud
                p.setPen(QPen(QColor(0, 180, 255, 160), baseSize, Qt::SolidLine, Qt::SquareCap));
                p.drawPoints(pts.data(), static_cast<int>(pts.size()));

                // Pass 2: Head particle burst (latest 15% of samples)
                size_t headStart = static_cast<size_t>(pts.size() * 0.85);
                if (headStart < pts.size()) {
                    int headCount = static_cast<int>(pts.size() - headStart);
                    double headSize = baseSize * 1.5;
                    p.setPen(QPen(QColor(80, 255, 210, 230), headSize, Qt::SolidLine, Qt::SquareCap));
                    p.drawPoints(pts.data() + headStart, headCount);
                }
            }
        }
    }
}
