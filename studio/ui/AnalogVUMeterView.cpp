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

void AnalogVUMeterView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_levelState) m_levelState->visibilityCount++;
}

void AnalogVUMeterView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_levelState && m_levelState->visibilityCount > 0) m_levelState->visibilityCount--;
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

    // SwiftUI AnalogVUMeter uses startAngle = 235 deg, endAngle = 305 deg (70 deg total span)
    // Relative to top vertical (0 deg), needle ranges from -35.0 deg to +35.0 deg.
    double startAngle = -35.0;
    double totalSpan = 70.0;
    return static_cast<float>(startAngle + clippedNorm * totalSpan);
}

void AnalogVUMeterView::setLevelDB(float leftDB, float rightDB) {
    m_leftDB = leftDB;
    m_rightDB = rightDB;

    if (leftDB >= -0.1f) m_peakClipLHold = 1.0f;
    if (rightDB >= -0.1f) m_peakClipRHold = 1.0f;

    m_targetAngleL = computeAngleForLevel(leftDB);
    m_targetAngleR = computeAngleForLevel(rightDB);
}

void AnalogVUMeterView::onAnimTick() {
    constexpr float alpha = 0.05404f;
    float nextL = m_currentAngleL + (m_targetAngleL - m_currentAngleL) * alpha;
    float nextR = m_currentAngleR + (m_targetAngleR - m_currentAngleR) * alpha;

    bool needUpdate = false;
    if (std::abs(nextL - m_currentAngleL) > 0.001f || std::abs(nextR - m_currentAngleR) > 0.001f) {
        m_currentAngleL = nextL;
        m_currentAngleR = nextR;
        needUpdate = true;
    }
    if (m_peakClipLHold > 0.0f) {
        m_peakClipLHold = std::max(0.0f, m_peakClipLHold - 0.02f);
        needUpdate = true;
    }
    if (m_peakClipRHold > 0.0f) {
        m_peakClipRHold = std::max(0.0f, m_peakClipRHold - 0.02f);
        needUpdate = true;
    }

    if (needUpdate) {
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

    drawSingleVU(p, QRect(8, 8, halfW, h - 16), m_currentAngleL, "LEFT", m_peakClipLHold > 0.0f);
    drawSingleVU(p, QRect(16 + halfW, 8, halfW, h - 16), m_currentAngleR, "RIGHT", m_peakClipRHold > 0.0f);
}

void AnalogVUMeterView::drawSingleVU(QPainter& p, const QRect& rect, float angleDeg, const QString& label, bool isClipped) {
    if (rect.width() < 20 || rect.height() < 20) return;

    p.save();
    p.setClipRect(rect);

    QColor bgTop, bgBot, textColor, arcPenColor, redPenColor, needlePenColor, bulbAmberColor, bulbHotSpotColor;
    if (m_settings.theme == VUTheme::VintageAmber) {
        bgTop = QColor("#f4ecd8"); bgBot = QColor("#dfd3b6");
        textColor = QColor("#3d2f21"); arcPenColor = QColor(61, 47, 33, 150);
        redPenColor = QColor(204, 41, 41, 204); needlePenColor = QColor(17, 17, 17, 230);
        bulbAmberColor = QColor(255, 209, 102); bulbHotSpotColor = QColor(255, 250, 224);
    } else if (m_settings.theme == VUTheme::DarkStealth) {
        bgTop = QColor("#1c1c1e"); bgBot = QColor("#0c0c0e");
        textColor = QColor("#e5e5ea"); arcPenColor = QColor(255, 255, 255, 76);
        redPenColor = QColor(255, 255, 255, 128); needlePenColor = QColor("#ffffff");
        bulbAmberColor = QColor(0, 0, 0, 100); bulbHotSpotColor = QColor(255, 255, 255, 38);
    } else { // Warm Tube
        bgTop = QColor("#2c1b12"); bgBot = QColor("#1a0e08");
        textColor = QColor("#ff9f0a"); arcPenColor = QColor(255, 159, 10, 128);
        redPenColor = QColor(217, 51, 26, 204); needlePenColor = QColor(38, 38, 38);
        bulbAmberColor = QColor(242, 115, 26); bulbHotSpotColor = QColor(255, 204, 77);
    }

    // Dial face background
    QRadialGradient faceGrad(rect.center(), std::max(1.0, rect.width() / 2.0));
    faceGrad.setColorAt(0.0, bgTop);
    faceGrad.setColorAt(1.0, bgBot);
    p.fillRect(rect, faceGrad);

    double effectiveRadiusScale = m_settings.radiusScale;
    double effectivePivotY = m_settings.pivotY;
    double effectiveNeedleExt = m_settings.needleExtension;

    QPointF pivot(rect.center().x(), rect.bottom() * effectivePivotY - rect.height() * (effectivePivotY - 1.0));
    double radius = rect.height() * 0.85 * effectiveRadiusScale;

    // Focused Bulb Hot Spot Glow Shading
    if (m_settings.hotSpotAlpha > 0) {
        QRadialGradient bulbGrad(QPointF(rect.center().x(), rect.bottom() + 5), rect.height() * 0.4);
        QColor hsColor = bulbHotSpotColor;
        hsColor.setAlphaF(m_settings.hotSpotAlpha);
        bulbGrad.setColorAt(0.0, hsColor);
        hsColor.setAlphaF(0.0);
        bulbGrad.setColorAt(1.0, hsColor);
        p.fillRect(rect, bulbGrad);
    }

    // Ambient Glow
    if (m_settings.ambientGlow > 0) {
        QRadialGradient glowGrad(QPointF(rect.center().x(), rect.bottom() + 10), rect.height() * 1.6);
        QColor ambColor = bulbAmberColor;
        ambColor.setAlphaF(m_settings.ambientGlow);
        glowGrad.setColorAt(0.0, ambColor);
        ambColor.setAlphaF(0.0);
        glowGrad.setColorAt(0.8, ambColor);
        glowGrad.setColorAt(1.0, ambColor);
        p.fillRect(rect, glowGrad);
    }

    // Main Scale Arc (-35 deg to +35 deg from North, mapped to 55 deg .. 125 deg CCW in Qt)
    // 90 deg North - 35 deg = 55 deg CCW; 90 deg + 35 deg = 125 deg CCW.
    p.setPen(QPen(arcPenColor, 1.8));
    p.drawArc(QRectF(pivot.x() - radius, pivot.y() - radius, radius * 2, radius * 2), 55 * 16, 70 * 16);

    // Red zone (> 0 VU / -18 dBFS)
    float zeroVUAngle = computeAngleForLevel(-18.0f); // ~ +13.02 deg right of North => 76.98 deg CCW
    double zeroVU_CCW = 90.0 - zeroVUAngle;
    double redSpan_CCW = zeroVU_CCW - 55.0; // from 0 VU to +3 VU (55 deg CCW)
    p.setPen(QPen(redPenColor, 4));
    p.drawArc(QRectF(pivot.x() - radius - 2, pivot.y() - radius - 2, (radius + 2) * 2, (radius + 2) * 2), 55 * 16, static_cast<int>(redSpan_CCW * 16));

    // Numeric Arc Markings & Ticks (-20 to +3 VU)
    p.setFont(QFont("sans-serif", 8, QFont::Bold));
    struct VUMark { double vu; const char* text; };
    static const VUMark marks[] = {
        {-20, "20"}, {-10, "10"}, {-7, "7"}, {-5, "5"}, {-3, "3"},
        {-2, "2"}, {-1, "1"}, {0, "0"}, {1, "1"}, {2, "2"}, {3, "3"}
    };

    for (const auto& m : marks) {
        float angle = computeAngleForLevel(-18.0f + m.vu);
        double rad = (angle - 90.0) * M_PI / 180.0;

        double xInner = pivot.x() + radius * std::cos(rad);
        double yInner = pivot.y() + radius * std::sin(rad);
        double xOuter = pivot.x() + (radius + 7) * std::cos(rad);
        double yOuter = pivot.y() + (radius + 7) * std::sin(rad);

        p.setPen(QPen(m.vu >= 0 ? redPenColor : arcPenColor, 1.8));
        p.drawLine(QPointF(xInner, yInner), QPointF(xOuter, yOuter));

        double xTxt = pivot.x() + (radius + 18) * std::cos(rad);
        double yTxt = pivot.y() + (radius + 18) * std::sin(rad);
        p.setPen(m.vu >= 0 ? redPenColor : textColor);
        p.drawText(QRectF(xTxt - 12, yTxt - 6, 24, 12), Qt::AlignCenter, m.text);
    }

    // Percentage Markings below main arc: [0, 20, 40, 60, 80, 100]
    p.setPen(QPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 100), 1.0));
    for (int pct : {0, 20, 40, 60, 80, 100}) {
        double ratio = pct / 100.0;
        double norm = (ratio - 0.1) / (1.412 - 0.1);
        double angle = -35.0 + norm * 70.0;
        double rad = (angle - 90.0) * M_PI / 180.0;

        double xOuter = pivot.x() + radius * std::cos(rad);
        double yOuter = pivot.y() + radius * std::sin(rad);
        double xInner = pivot.x() + (radius - 7) * std::cos(rad);
        double yInner = pivot.y() + (radius - 7) * std::sin(rad);

        p.drawLine(QPointF(xInner, yInner), QPointF(xOuter, yOuter));

        double xTxt = pivot.x() + (radius - 18) * std::cos(rad);
        double yTxt = pivot.y() + (radius - 18) * std::sin(rad);
        p.drawText(QRectF(xTxt - 12, yTxt - 6, 24, 12), Qt::AlignCenter, QString::number(pct));
    }

    // Dynamic Needle
    p.save();
    p.translate(pivot);
    p.rotate(angleDeg);

    // Dynamic Needle Drop Shadow (offset for realistic depth according to theme)
    QColor needleShadowColor;
    if (m_settings.theme == VUTheme::VintageAmber) {
        needleShadowColor = QColor(61, 47, 33, 75);
    } else if (m_settings.theme == VUTheme::DarkStealth) {
        needleShadowColor = QColor(0, 0, 0, 140);
    } else { // Warm Tube
        needleShadowColor = QColor(20, 10, 5, 120);
    }

    double nLen = radius + effectiveNeedleExt;
    p.setPen(QPen(needleShadowColor, 1.6));
    p.drawLine(QPointF(1.5, 1.5), QPointF(1.5, -nLen + 1.5));

    p.setPen(QPen(needlePenColor, 1.2));
    p.drawLine(0, 0, 0, -nLen);

    // Pivot cap
    p.setBrush(QColor("#2b2b2b"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(0, 0), 6, 6);

    p.restore();

    // Peak Clip Indicator Lamp (Top Right)
    QPointF ledPos(rect.right() - 20, rect.top() + 20);
    p.setPen(QPen(QColor("#111111"), 1));
    if (isClipped) {
        QRadialGradient clipGlow(ledPos, 12);
        clipGlow.setColorAt(0.0, QColor(255, 50, 50, 255));
        clipGlow.setColorAt(0.5, QColor(255, 0, 0, 200));
        clipGlow.setColorAt(1.0, QColor(255, 0, 0, 0));
        p.fillRect(QRectF(ledPos.x() - 12, ledPos.y() - 12, 24, 24), clipGlow);
        p.setBrush(QColor("#ff0000"));
    } else {
        p.setBrush(QColor("#4a1111"));
    }
    p.drawEllipse(ledPos, 5, 5);

    // Additive Light Wash
    if (m_settings.lightWash > 0) {
        QColor lwColor = bulbAmberColor;
        lwColor.setAlphaF(m_settings.lightWash);
        p.fillRect(rect, lwColor);
    }

    // Glass Surface Glare Reflection Overlay
    QLinearGradient glassGrad(rect.topLeft(), rect.bottomRight());
    glassGrad.setColorAt(0.0, QColor(255, 255, 255, 60));
    glassGrad.setColorAt(0.4, QColor(255, 255, 255, 10));
    glassGrad.setColorAt(0.5, QColor(255, 255, 255, 0));
    glassGrad.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.fillRect(rect, glassGrad);

    // Border
    p.setPen(QPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 50), 1.2));
    p.drawRoundedRect(rect, 6, 6);

    // Label
    p.setFont(QFont("sans-serif", 10, QFont::Bold));
    p.setPen(textColor);
    p.drawText(rect.center().x() - 20, rect.bottom() - 15, label);

    p.restore();
}
