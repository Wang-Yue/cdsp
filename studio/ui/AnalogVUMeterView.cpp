#include "ui/AnalogVUMeterView.h"
#include <cmath>
#include <algorithm>
#include <QRadialGradient>
#include <QLinearGradient>

AnalogVUMeterView::AnalogVUMeterView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    connect(&m_animTimer, &QTimer::timeout, this, &AnalogVUMeterView::onAnimTick);
    m_animTimer.start(16); // ~60 FPS continuous ballistic animation
}

void AnalogVUMeterView::setVUSettings(const VUSettings& settings) {
    m_settings = settings;
    update();
}

float AnalogVUMeterView::computeAngleForLevel(float dbFS) const {
    double level = static_cast<double>(dbFS);
    double refLevel = -18.0; // 0 VU = -18 dBFS
    double vu = level - refLevel;

    double ratio = std::pow(10.0, vu / 20.0);
    double minR = 0.1;
    double maxR = 1.412;
    double norm = (ratio - minR) / (maxR - minR);
    double clippedNorm = std::min(std::max(norm, -0.076), 1.1);

    double startAngle = -45.0;
    double totalSpan = 90.0;
    return static_cast<float>(startAngle + clippedNorm * totalSpan);
}

void AnalogVUMeterView::setLevelDB(float leftDB, float rightDB) {
    m_leftDB = leftDB;
    m_rightDB = rightDB;

    m_targetAngleL = computeAngleForLevel(leftDB);
    m_targetAngleR = computeAngleForLevel(rightDB);
}

void AnalogVUMeterView::onAnimTick() {
    // 60 FPS Ballistic spring inertia physics update
    m_currentAngleL += (m_targetAngleL - m_currentAngleL) * 0.18f;
    m_currentAngleR += (m_targetAngleR - m_currentAngleR) * 0.18f;

    if (std::abs(m_targetAngleL - m_currentAngleL) > 0.01f || std::abs(m_targetAngleR - m_currentAngleR) > 0.01f) {
        update();
    }
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

    QColor bgTop, bgBot, textColor, arcPenColor, redPenColor, needlePenColor;
    if (m_settings.theme == VUTheme::VintageAmber) {
        bgTop = QColor("#f4ecd8"); bgBot = QColor("#dfd3b6");
        textColor = QColor("#3d2f21"); arcPenColor = QColor("#1b1b1b");
        redPenColor = QColor("#cc2929"); needlePenColor = QColor("#111111");
    } else if (m_settings.theme == VUTheme::DarkStealth) {
        bgTop = QColor("#1c1c1e"); bgBot = QColor("#0c0c0e");
        textColor = QColor("#e5e5ea"); arcPenColor = QColor("#8e8e93");
        redPenColor = QColor("#ff453a"); needlePenColor = QColor("#ffffff");
    } else { // Warm Tube
        bgTop = QColor("#2c1b12"); bgBot = QColor("#1a0e08");
        textColor = QColor("#ff9f0a"); arcPenColor = QColor("#ff9f0a");
        redPenColor = QColor("#ff3b30"); needlePenColor = QColor("#ff9f0a");
    }

    // Vintage Amber / Stealth Background
    QRadialGradient faceGrad(rect.center(), rect.width() / 2);
    faceGrad.setColorAt(0.0, bgTop);
    faceGrad.setColorAt(1.0, bgBot);
    p.fillRect(rect, faceGrad);
    p.setPen(QPen(QColor("#4a3b2c"), 2));
    p.drawRect(rect);

    QPointF pivot(rect.center().x(), rect.bottom() * m_settings.pivotY - rect.height() * (m_settings.pivotY - 1.0));
    double radius = rect.height() * 0.85 * m_settings.radiusScale;

    // Focused Bulb Hot Spot Glow Shading
    QRadialGradient bulbGrad(QPointF(rect.center().x(), rect.bottom() - 10), rect.height() * 0.6);
    bulbGrad.setColorAt(0.0, QColor(255, 220, 120, static_cast<int>(255 * m_settings.hotSpotAlpha)));
    bulbGrad.setColorAt(1.0, QColor(255, 180, 50, 0));
    p.fillRect(rect, bulbGrad);

    // Ambient Warm Glow
    QRadialGradient glowGrad(QPointF(rect.center().x(), rect.bottom()), rect.height() * 1.2);
    glowGrad.setColorAt(0.0, QColor(255, 180, 50, static_cast<int>(255 * m_settings.ambientGlow)));
    glowGrad.setColorAt(0.8, QColor(255, 180, 50, 0));
    p.fillRect(rect, glowGrad);

    // Overall Light Wash
    if (m_settings.lightWash > 0) {
        p.fillRect(rect, QColor(255, 240, 200, static_cast<int>(255 * m_settings.lightWash)));
    }

    // Scale Arc
    p.setPen(QPen(arcPenColor, 2));
    p.drawArc(QRectF(pivot.x() - radius, pivot.y() - radius, radius * 2, radius * 2), 45 * 16, 90 * 16);

    // Red zone (> 0 dB / 0 VU)
    p.setPen(QPen(redPenColor, 3));
    p.drawArc(QRectF(pivot.x() - radius, pivot.y() - radius, radius * 2, radius * 2), 45 * 16, 20 * 16);

    // Numeric Arc Markings & Ticks (-20 to +3 VU)
    p.setFont(QFont("sans-serif", 8, QFont::Bold));
    struct VUMark { double vu; const char* text; };
    static const VUMark marks[] = {
        {-20, "-20"}, {-10, "-10"}, {-7, "-7"}, {-5, "-5"}, {-3, "-3"},
        {-2, "-2"}, {-1, "-1"}, {0, "0"}, {1, "+1"}, {2, "+2"}, {3, "+3"}
    };

    for (const auto& m : marks) {
        float angle = computeAngleForLevel(-18.0f + m.vu);
        double rad = (angle - 90.0) * M_PI / 180.0;

        double xInner = pivot.x() + (radius - 6) * std::cos(rad);
        double yInner = pivot.y() + (radius - 6) * std::sin(rad);
        double xOuter = pivot.x() + (radius + 4) * std::cos(rad);
        double yOuter = pivot.y() + (radius + 4) * std::sin(rad);

        p.setPen(QPen(m.vu >= 0 ? redPenColor : arcPenColor, m.vu == 0 ? 2 : 1));
        p.drawLine(QPointF(xInner, yInner), QPointF(xOuter, yOuter));

        double xTxt = pivot.x() + (radius - 16) * std::cos(rad);
        double yTxt = pivot.y() + (radius - 16) * std::sin(rad);
        p.setPen(m.vu >= 0 ? redPenColor : textColor);
        p.drawText(QRectF(xTxt - 12, yTxt - 6, 24, 12), Qt::AlignCenter, m.text);
    }

    // Dynamic Needle
    p.save();
    p.translate(pivot);
    p.rotate(angleDeg);

    p.setPen(QPen(needlePenColor, 2));
    double nLen = radius * 0.9 + m_settings.needleExtension;
    p.drawLine(0, 0, 0, -nLen);

    // Pivot cap
    p.setBrush(QColor("#2b2b2b"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(0, 0), 8, 8);

    p.restore();

    // Glass Surface Glare Reflection Overlay
    QLinearGradient glassGrad(rect.topLeft(), rect.bottomRight());
    glassGrad.setColorAt(0.0, QColor(255, 255, 255, 30));
    glassGrad.setColorAt(0.4, QColor(255, 255, 255, 10));
    glassGrad.setColorAt(0.5, QColor(255, 255, 255, 0));
    p.fillRect(rect, glassGrad);

    // Labels
    p.setFont(QFont("sans-serif", 10, QFont::Bold));
    p.setPen(textColor);
    p.drawText(rect.center().x() - 20, rect.top() + 30, label);
    p.drawText(rect.center().x() - 10, rect.top() + 50, "VU");

    p.restore();
}
