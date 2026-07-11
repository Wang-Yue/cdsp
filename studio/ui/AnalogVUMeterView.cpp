#include "ui/AnalogVUMeterView.h"
#include <cmath>
#include <algorithm>
#include <QRadialGradient>

AnalogVUMeterView::AnalogVUMeterView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

void AnalogVUMeterView::setLevelDB(float leftDB, float rightDB) {
    auto dbToAngle = [](float db) -> float {
        float clamped = std::max(-20.0f, std::min(3.0f, db));
        float norm = (clamped + 20.0f) / 23.0f;
        return -45.0f + norm * 90.0f; // -45 deg to +45 deg
    };

    float targetL = dbToAngle(leftDB);
    float targetR = dbToAngle(rightDB);

    // Ballistic inertia dampening
    m_currentAngleL += (targetL - m_currentAngleL) * 0.25f;
    m_currentAngleR += (targetR - m_currentAngleR) * 0.25f;

    m_leftDB = leftDB;
    m_rightDB = rightDB;

    update();
}

void AnalogVUMeterView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    int halfW = (w - 24) / 2;

    drawSingleVU(p, QRect(8, 8, halfW, h - 16), m_currentAngleL, "LEFT");
    drawSingleVU(p, QRect(16 + halfW, 8, halfW, h - 16), m_currentAngleR, "RIGHT");
}

void AnalogVUMeterView::drawSingleVU(QPainter& p, const QRect& rect, float angleDeg, const QString& label) {
    p.save();
    p.setClipRect(rect);

    // Cream / Vintage Warm Face Card
    QRadialGradient faceGrad(rect.center(), rect.width() / 2);
    faceGrad.setColorAt(0.0, QColor("#f4ecd8"));
    faceGrad.setColorAt(1.0, QColor("#dfd3b6"));
    p.fillRect(rect, faceGrad);
    p.setPen(QPen(QColor("#4a3b2c"), 2));
    p.drawRect(rect);

    QPointF pivot(rect.center().x(), rect.bottom() + 20);
    double radius = rect.height() * 0.85;

    // Scale Arc
    p.setPen(QPen(QColor("#1b1b1b"), 2));
    p.drawArc(QRectF(pivot.x() - radius, pivot.y() - radius, radius * 2, radius * 2), 45 * 16, 90 * 16);

    // Red zone (> 0 dB)
    p.setPen(QPen(QColor("#cc2929"), 3));
    p.drawArc(QRectF(pivot.x() - radius, pivot.y() - radius, radius * 2, radius * 2), 45 * 16, 20 * 16);

    // Labels
    p.setFont(QFont("sans-serif", 10, QFont::Bold));
    p.setPen(QColor("#3d2f21"));
    p.drawText(rect.center().x() - 20, rect.top() + 30, label);
    p.drawText(rect.center().x() - 10, rect.top() + 50, "VU");

    // Dynamic Needle
    p.translate(pivot);
    p.rotate(angleDeg);

    p.setPen(QPen(QColor("#111111"), 2));
    p.drawLine(0, 0, 0, -radius * 0.9);

    // Pivot cap
    p.setBrush(QColor("#2b2b2b"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(0, 0), 8, 8);

    p.restore();
}
