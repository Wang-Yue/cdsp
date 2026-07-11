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
            if (m_engine) setSamples(m_engine->samples, m_engine->showParticles);
        });
        setSamples(m_engine->samples, m_engine->showParticles);
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

void VectorScopeView::setSamples(const AudioSamplesData& samples, bool showParticles) {
    m_samples = samples;
    m_showParticles = showParticles;
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

    float maxVal = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float m = (left[i] + right[i]) * 0.7071f;
        float s = (left[i] - right[i]) * 0.7071f;
        maxVal = std::max(maxVal, std::max(std::abs(m), std::abs(s)));
    }
    float autoScaleFactor = (maxVal > 1e-4f) ? std::min(0.85f / maxVal, 32.0f) : 1.0f;

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
        // Particle Mode with Indigo-Cyan gradient, tail age decay, and head glowing halo
        p.setPen(Qt::NoPen);

        for (size_t i = 0; i < count; ++i) {
            float l = left[i];
            float r = right[i];
            float t = static_cast<float>(i) / static_cast<float>(count - 1);

            float m = (l + r) * 0.7071f;
            float s = (l - r) * 0.7071f;

            float x = centerPt.x() + s * (center * autoScaleFactor);
            float y = centerPt.y() - m * (center * autoScaleFactor);

            float radius = 1.0f + 3.5f * t;
            QColor col = QColor::fromHsvF(0.65f - 0.2f * t, 0.8f, 0.9f, 0.05f + 0.85f * t);

            p.setBrush(col);
            p.drawEllipse(QPointF(x, y), radius, radius);

            // Glowing head halo for recent samples
            if (i + 10 >= count) {
                p.setBrush(QColor(255, 255, 255, 120));
                p.drawEllipse(QPointF(x, y), radius + 2.0, radius + 2.0);
            }
        }
    }
}
