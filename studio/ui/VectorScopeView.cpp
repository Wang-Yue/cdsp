#include "ui/VectorScopeView.h"
#include "ui/StyleTheme.h"
#include <cmath>
#include <QPainterPath>

VectorScopeView::VectorScopeView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

VectorScopeView::VectorScopeView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(180);
    setEngine(engine);
}

void VectorScopeView::setEngine(std::shared_ptr<VectorScopeEngine> engine) {
    m_engine = engine;
    if (m_engine) {
        connect(m_engine.get(), &VectorScopeEngine::updated, this, [this]() {
            if (m_engine) setSamples(m_engine->samples, m_engine->showParticles, m_engine->autoScale);
        });
        setSamples(m_engine->samples, m_engine->showParticles, m_engine->autoScale);
    }
}

void VectorScopeView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_engine) m_engine->visibilityCount++;
}

void VectorScopeView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0) m_engine->visibilityCount--;
}

void VectorScopeView::setSamples(const AudioSamplesData& samples, bool showParticles, bool autoScale) {
    m_samples = samples;
    m_showParticles = showParticles;
    m_autoScale = autoScale;
    update();
}

void VectorScopeView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    int w = width();
    int h = height();
    int center = std::min(w, h) / 2;
    QPoint centerPt(w / 2, h / 2);

    // Reticle axes (+M, -M, +S, -S) & Diagonal Corner-to-Corner X grid lines
    p.setPen(QPen(QColor(255, 255, 255, 25), 1, Qt::DashLine));
    p.drawLine(centerPt.x() - center, centerPt.y(), centerPt.x() + center, centerPt.y());
    p.drawLine(centerPt.x(), centerPt.y() - center, centerPt.x(), centerPt.y() + center);

    // Diagonal X grid lines
    int offset = static_cast<int>(center * 0.7071);
    p.drawLine(centerPt.x() - offset, centerPt.y() - offset, centerPt.x() + offset, centerPt.y() + offset);
    p.drawLine(centerPt.x() - offset, centerPt.y() + offset, centerPt.x() + offset, centerPt.y() - offset);

    p.drawEllipse(centerPt, center * 3 / 4, center * 3 / 4);

    const auto& left = m_samples.left();
    const auto& right = m_samples.right();
    size_t count = std::min(left.size(), right.size());
    if (count == 0) return;

    bool enableAutoScale = m_engine ? m_engine->autoScale : m_autoScale;
    float autoScaleFactor = 1.0f;
    if (enableAutoScale) {
        float maxVal = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            float m = (left[i] + right[i]) * 0.7071f;
            float s = (left[i] - right[i]) * 0.7071f;
            maxVal = std::max(maxVal, std::max(std::abs(m), std::abs(s)));
        }
        autoScaleFactor = (maxVal > 1e-4f) ? std::min(0.90f / maxVal, 32.0f) : 1.0f;
    }

    if (!m_showParticles) {
        QPainterPath path;
        for (size_t i = 0; i < count; ++i) {
            float l = left[i];
            float r = right[i];

            float m = (l + r) * 0.7071f;
            float s = (l - r) * 0.7071f;

            float x = centerPt.x() + s * (center * autoScaleFactor);
            float y = centerPt.y() - m * (center * autoScaleFactor);

            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        p.setPen(QPen(QColor("#34c759"), 1.5));
        p.drawPath(path);
    } else {
        // Particle Mode matching SwiftUI: Indigo to Cyan HSV gradient, head halo, particle radius decay
        p.setPen(Qt::NoPen);

        for (size_t i = 0; i < count; ++i) {
            float l = left[i];
            float r = right[i];
            float t = (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : 1.0f;

            float m = (l + r) * 0.7071f;
            float s = (l - r) * 0.7071f;

            float x = centerPt.x() + s * (center * autoScaleFactor);
            float y = centerPt.y() - m * (center * autoScaleFactor);

            float size = 1.0f + 3.5f * t;
            float alpha = 0.03f + 0.82f * t;

            // Indigo (HSV 0.65) to Cyan (HSV 0.50)
            float hue = 0.65f - 0.15f * t;
            QColor particleColor = QColor::fromHsvF(hue, 0.8f, 0.95f, alpha);

            // Glowing head halo for head of vector stream (t > 0.9)
            if (t > 0.9f) {
                float glowSize = size * 2.0f;
                QColor haloColor = particleColor;
                haloColor.setAlphaF(0.25f);
                p.setBrush(haloColor);
                p.drawEllipse(QPointF(x, y), glowSize / 2.0f, glowSize / 2.0f);
            }

            p.setBrush(particleColor);
            p.drawEllipse(QPointF(x, y), size / 2.0f, size / 2.0f);
        }
    }
}
