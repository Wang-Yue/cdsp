#include "ui/VectorScopeView.h"

#include <QEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

VectorScopeView::VectorScopeView(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumHeight(180);
}

VectorScopeView::VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent) : QOpenGLWidget(parent) {
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
    QOpenGLWidget::showEvent(event);
    if (m_engine)
        m_engine->visibilityCount++;
}

void VectorScopeView::hideEvent(QHideEvent* event) {
    QOpenGLWidget::hideEvent(event);
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
    QOpenGLWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        update();
    }
}

void VectorScopeView::paintGL() {
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

    int margin = 16;
    int drawH = h - 2 * margin;
    int centerRadius = std::max(10, std::min(w - 2 * margin, drawH) / 2);
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
            // Line Mode: Direct GPU polyline drawing
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
                p.setPen(QPen(QColor(0, 160, 255, 210), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPolyline(pts.data(), static_cast<int>(pts.size()));
            }
        } else {
            // Particle Mode: Direct GPU batched drawing with palette
            constexpr int NUM_BINS = 32;
            static const auto binColors = []() {
                std::array<QColor, NUM_BINS> cols;
                for (int b = 0; b < NUM_BINS; ++b) {
                    float t = static_cast<float>(b) / static_cast<float>(NUM_BINS - 1);
                    float alpha = 0.15f + 0.80f * t;
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

            p.setPen(Qt::NoPen);

            // Draw main particles by bin
            for (int b = 0; b < NUM_BINS; ++b) {
                if (binPoints[b].empty())
                    continue;
                float t = static_cast<float>(b) / static_cast<float>(NUM_BINS - 1);
                float particleSize = 1.2f + 3.0f * t;
                float halfSize = particleSize / 2.0f;

                p.setBrush(binColors[b]);
                for (const auto& pt : binPoints[b]) {
                    p.drawEllipse(pt, halfSize, halfSize);
                }
            }

            // Draw glow halos for head of stream
            if (!glowPoints.empty()) {
                p.setBrush(QColor(binColors[NUM_BINS - 1].red(), binColors[NUM_BINS - 1].green(),
                                  binColors[NUM_BINS - 1].blue(), 70));
                for (const auto& pt : glowPoints) {
                    p.drawEllipse(pt, 4.0f, 4.0f);
                }
            }
        }
    }
}
