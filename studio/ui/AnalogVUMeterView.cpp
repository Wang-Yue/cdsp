#include "ui/AnalogVUMeterView.h"

#include "ui/StyleTheme.h"

#include <QEvent>
#include <QFont>
#include <QLinearGradient>
#include <QPainterPath>
#include <QRadialGradient>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AnalogVUMeterView::AnalogVUMeterView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);

    m_physicsTimer = new QTimer(this);
    connect(m_physicsTimer, &QTimer::timeout, this, &AnalogVUMeterView::updateNeedlePhysics);
    m_physicsTimer->start(16); // ~60 FPS
}

void AnalogVUMeterView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_levelState)
        m_levelState->visibilityCount++;
}

void AnalogVUMeterView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_levelState && m_levelState->visibilityCount > 0)
        m_levelState->visibilityCount--;
}

void AnalogVUMeterView::setVUSettings(const VUSettings& settings) {
    m_settings = settings;
    update();
}

float AnalogVUMeterView::computeAngleForLevel(float dbFS) const {
    double level = static_cast<double>(dbFS);
    double refLevel = -18.0; // 0 VU = -18 dBFS (matches SwiftUI refLevel)
    double vu = level - refLevel;

    double ratio = std::pow(10.0, vu / 20.0);
    double minR = 0.1;
    double maxR = 1.412;
    double norm = (ratio - minR) / (maxR - minR);
    double clippedNorm = std::min(std::max(norm, -0.076), 1.1);

    double startAngle = -35.0; // Relative to North (12 o'clock)
    double totalSpan = 70.0;
    return static_cast<float>(startAngle + clippedNorm * totalSpan);
}

void AnalogVUMeterView::setLevelDB(float leftDB, float rightDB) {
    m_leftDB = leftDB;
    m_rightDB = rightDB;
    update();
}

void AnalogVUMeterView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        update();
    }
}

void AnalogVUMeterView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    size_t chCount = 2;
    if (m_levelState && !m_levelState->playbackRms.empty()) {
        chCount = std::max(static_cast<size_t>(1), m_levelState->playbackRms.size());
    }

    int spacing = 16;
    int meterWidth = (w - static_cast<int>(chCount + 1) * spacing) / static_cast<int>(chCount);
    meterWidth = std::max(40, meterWidth);

    for (size_t i = 0; i < chCount; ++i) {
        float levelDb = -100.0f;
        if (m_levelState && i < m_levelState->playbackRms.size()) {
            levelDb = m_levelState->playbackRms[i];
        } else {
            levelDb = (i == 0) ? m_leftDB : m_rightDB;
        }

        float angle = -35.0f;
        if (i < m_currentAngles.size()) {
            angle = m_currentAngles[i];
        } else {
            angle = computeAngleForLevel(levelDb);
        }

        int xPos = spacing + static_cast<int>(i) * (meterWidth + spacing);
        QRect totalRect(xPos, 4, meterWidth, h - 8);

        float scale = totalRect.height() / 160.0f;
        drawSingleVU(p, totalRect, angle, QString::number(i + 1), scale);
    }
}

void AnalogVUMeterView::drawSingleVU(QPainter& p, const QRect& totalRect, float angleDeg, const QString& label,
                                     float scale) {
    if (totalRect.width() < 20 || totalRect.height() < 20)
        return;

    p.save();

    QRect innerRect = totalRect.adjusted(static_cast<int>(4 * scale), static_cast<int>(4 * scale),
                                         -static_cast<int>(4 * scale), -static_cast<int>(4 * scale));
    int vSpacing = static_cast<int>(6 * scale);

    QFont labelFont("sans-serif", static_cast<int>(std::max(8.0, 11.0 * scale)), QFont::Black);
    QFontMetrics labelFm(labelFont);
    int labelHeight = labelFm.height();

    QRect dialRect(innerRect.left(), innerRect.top(), innerRect.width(), innerRect.height() - labelHeight - vSpacing);
    QRect labelRect(innerRect.left(), dialRect.bottom() + vSpacing, innerRect.width(), labelHeight);

    QColor bulbAmberColor, bulbHotSpotColor, needleColor, arcColor, percentageMarksColor, redZoneColor;
    bool isDark = StyleTheme::isDark();

    if (m_settings.theme == VUTheme::VintageAmber) {
        bulbAmberColor = QColor(255, 209, 102);   // Color(red: 1.0, green: 0.82, blue: 0.40)
        bulbHotSpotColor = QColor(255, 250, 224); // Color(red: 1.0, green: 0.98, blue: 0.88)
        needleColor = isDark ? QColor(255, 255, 255, 230) : QColor(0, 0, 0, 230);          // .primary.opacity(0.9)
        arcColor = isDark ? QColor(255, 255, 255, 153) : QColor(0, 0, 0, 153);             // .primary.opacity(0.6)
        percentageMarksColor = isDark ? QColor(255, 255, 255, 102) : QColor(0, 0, 0, 102); // .primary.opacity(0.4)
        redZoneColor = QColor(255, 0, 0, 204);                                             // .red.opacity(0.8)
    } else if (m_settings.theme == VUTheme::DarkStealth) {
        bulbAmberColor = QColor(0, 0, 0, 102);                                           // Color.black.opacity(0.4)
        bulbHotSpotColor = QColor(255, 255, 255, 38);                                    // Color.white.opacity(0.15)
        needleColor = QColor(255, 255, 255);                                             // .white
        arcColor = isDark ? QColor(255, 255, 255, 76) : QColor(0, 0, 0, 76);             // .primary.opacity(0.3)
        percentageMarksColor = isDark ? QColor(255, 255, 255, 51) : QColor(0, 0, 0, 51); // .primary.opacity(0.2)
        redZoneColor = isDark ? QColor(255, 255, 255, 128) : QColor(0, 0, 0, 128);       // .primary.opacity(0.5)
    } else {                                                                             // Warm Tube
        bulbAmberColor = QColor(242, 115, 26);   // Color(red: 0.95, green: 0.45, blue: 0.1)
        bulbHotSpotColor = QColor(255, 204, 77); // Color(red: 1.0, green: 0.8, blue: 0.3)
        needleColor = QColor(38, 38, 38);        // Color(red: 0.15, green: 0.15, blue: 0.15)
        arcColor = isDark ? QColor(255, 255, 255, 128) : QColor(0, 0, 0, 128);           // .primary.opacity(0.5)
        percentageMarksColor = isDark ? QColor(255, 255, 255, 76) : QColor(0, 0, 0, 76); // .primary.opacity(0.3)
        redZoneColor = QColor(217, 51, 26, 204); // Color(red: 0.85, green: 0.2, blue: 0.1).opacity(0.8)
    }

    double w = dialRect.width();
    double h = dialRect.height();

    QPointF center(dialRect.left() + w / 2.0, dialRect.top() + h * m_settings.pivotY);
    double radius = h * m_settings.radiusScale;
    double baseH = dialRect.top() + h;

    p.save();
    QPainterPath clipPath;
    clipPath.addRoundedRect(dialRect, 6 * scale, 6 * scale);
    p.setClipPath(clipPath);

    // 1. BOTTOM AMBER GLOW
    if (m_settings.ambientGlow > 0) {
        QRadialGradient amberGlow(QPointF(dialRect.left() + w / 2.0, baseH + 10 * scale), h * 1.6);
        QColor ambColor = bulbAmberColor;
        ambColor.setAlphaF(m_settings.ambientGlow);
        amberGlow.setColorAt(0.0, ambColor);
        ambColor.setAlphaF(0.0);
        amberGlow.setColorAt(0.8, ambColor);
        amberGlow.setColorAt(1.0, ambColor);
        p.fillRect(dialRect, amberGlow);
    }

    // 2. HOT SPOT
    if (m_settings.hotSpotAlpha > 0) {
        QRadialGradient hotSpot(QPointF(dialRect.left() + w / 2.0, baseH + 5 * scale), h * 0.4);
        QColor hsColor = bulbHotSpotColor;
        hsColor.setAlphaF(m_settings.hotSpotAlpha);
        hotSpot.setColorAt(0.0, hsColor);
        hsColor.setAlphaF(0.0);
        hotSpot.setColorAt(1.0, hsColor);
        p.fillRect(dialRect, hotSpot);
    }

    // 3. Main Scale Arc
    p.setPen(QPen(arcColor, 1.8 * scale));
    p.drawArc(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2), 55 * 16, 70 * 16);

    // 4. Marks Drawing (-20 to +3 VU)
    struct VUMark {
        double vu;
        const char* text;
    };
    static const VUMark vuMarks[] = {{-20, "20"}, {-10, "10"}, {-7, "7"}, {-5, "5"}, {-3, "3"}, {-2, "2"},
                                     {-1, "1"},   {0, "0"},    {1, "1"},  {2, "2"},  {3, "3"}};

    QFont vintageFont("Rockwell", static_cast<int>(std::max(7.0, 10.0 * scale)), QFont::Normal);
    vintageFont.setStyleHint(QFont::Serif);

    for (const auto& m : vuMarks) {
        float angDeg = computeAngleForLevel(-18.0f + m.vu);
        double rad = (angDeg - 90.0) * M_PI / 180.0;

        double cosA = std::cos(rad);
        double sinA = std::sin(rad);

        QPointF s(center.x() + cosA * radius, center.y() + sinA * radius);
        double eR = radius + 7 * scale;
        QPointF e(center.x() + cosA * eR, center.y() + sinA * eR);

        QColor color = m.vu >= 0 ? redZoneColor : arcColor;
        QColor tickColor = color;
        tickColor.setAlphaF(color.alphaF() * 0.7);

        p.setPen(QPen(tickColor, 1.8 * scale));
        p.drawLine(s, e);

        double lR = radius + 18 * scale;
        QPointF lp(center.x() + cosA * lR, center.y() + sinA * lR);

        p.save();
        p.translate(lp);
        p.rotate(angDeg);
        p.setFont(vintageFont);
        QColor textColor = color;
        textColor.setAlphaF(color.alphaF() * 0.6);
        p.setPen(textColor);
        p.drawText(QRectF(-15 * scale, -8 * scale, 30 * scale, 16 * scale), Qt::AlignCenter, m.text);
        p.restore();
    }

    // Percentage Markings (BELOW main arc: 0 to 100)
    for (int pct : {0, 20, 40, 60, 80, 100}) {
        double ratio = pct / 100.0;
        double norm = (ratio - 0.1) / (1.412 - 0.1);
        double angDeg = -35.0 + norm * 70.0;
        double rad = (angDeg - 90.0) * M_PI / 180.0;

        double cosA = std::cos(rad);
        double sinA = std::sin(rad);

        QPointF s(center.x() + cosA * radius, center.y() + sinA * radius);
        double eR = radius - 7 * scale;
        QPointF e(center.x() + cosA * eR, center.y() + sinA * eR);

        QColor pTickColor = percentageMarksColor;
        pTickColor.setAlphaF(percentageMarksColor.alphaF() * 0.4);
        p.setPen(QPen(pTickColor, 1.0 * scale));
        p.drawLine(s, e);

        double lR = radius - 18 * scale;
        QPointF lp(center.x() + cosA * lR, center.y() + sinA * lR);

        p.save();
        p.translate(lp);
        p.rotate(angDeg);
        p.setFont(vintageFont);
        p.setPen(pTickColor);
        p.drawText(QRectF(-15 * scale, -8 * scale, 30 * scale, 16 * scale), Qt::AlignCenter, QString::number(pct));
        p.restore();
    }

    // 5. Red Zone Arc
    float redS = computeAngleForLevel(-18.0f); // 0 VU
    double zeroVU_CCW = 90.0 - redS;
    double redSpan_CCW = zeroVU_CCW - 55.0; // from 0 VU to +3 VU
    p.setPen(QPen(redZoneColor, 4 * scale));
    p.drawArc(QRectF(center.x() - radius - 2 * scale, center.y() - radius - 2 * scale, (radius + 2 * scale) * 2,
                     (radius + 2 * scale) * 2),
              55 * 16, static_cast<int>(redSpan_CCW * 16));

    // 6. Glass Surface Reflection
    QLinearGradient glassGrad(dialRect.topLeft(), dialRect.bottomRight());
    glassGrad.setColorAt(0.0, QColor(255, 255, 255, 64)); // white 0.25 opacity
    glassGrad.setColorAt(0.5, QColor(255, 255, 255, 0));  // clear
    glassGrad.setColorAt(1.0, QColor(0, 0, 0, 13));       // black 0.05 opacity
    p.fillRect(dialRect, glassGrad);

    // 7. ADDITIVE LIGHT WASH
    if (m_settings.lightWash > 0) {
        QColor lwColor = bulbAmberColor;
        lwColor.setAlphaF(m_settings.lightWash);
        p.fillRect(dialRect, lwColor);
    }

    // 8. Perfected Needle (rendered on top of dial face multi-layer shaders)
    double nAngRad = (angleDeg - 90.0) * M_PI / 180.0;
    double nR = radius + m_settings.needleExtension * scale;
    QPointF ne(center.x() + std::cos(nAngRad) * nR, center.y() + std::sin(nAngRad) * nR);

    p.setPen(QPen(needleColor, 1.2 * scale));
    p.drawLine(center, ne);

    p.restore(); // Restore clip region

    // Dial Box Outer Border Stroke & Corner Radius
    QPainterPath boxPath;
    boxPath.addRoundedRect(dialRect, 6 * scale, 6 * scale);
    p.setPen(QPen(isDark ? QColor(255, 255, 255, 51) : QColor(0, 0, 0, 51), 1.2 * scale));
    p.drawPath(boxPath);

    // Channel Label (Positioned below the dial box)
    p.setFont(labelFont);
    QColor lblColor = StyleTheme::textSecondary();
    lblColor.setAlphaF(lblColor.alphaF() * 0.8);
    p.setPen(lblColor);
    p.drawText(labelRect, Qt::AlignCenter, label);

    p.restore();
}

void AnalogVUMeterView::updateNeedlePhysics() {
    size_t chCount = 2;
    if (m_levelState && !m_levelState->playbackRms.empty()) {
        chCount = std::max(static_cast<size_t>(1), m_levelState->playbackRms.size());
    }

    if (m_currentAngles.size() != chCount) {
        m_currentAngles.resize(chCount, -35.0f);
        m_velocities.resize(chCount, 0.0f);
    }

    float dt = 0.016f; // ~60 FPS time step
    bool needsUpdate = false;

    for (size_t i = 0; i < chCount; ++i) {
        float levelDb = -100.0f;
        if (m_levelState && i < m_levelState->playbackRms.size()) {
            levelDb = m_levelState->playbackRms[i];
        } else {
            levelDb = (i == 0) ? m_leftDB : m_rightDB;
        }

        // Apply gain calibration offset
        levelDb += m_gainCalibrationDb;

        float targetAngle = computeAngleForLevel(levelDb);
        float diff = targetAngle - m_currentAngles[i];

        // Second-order spring-damper dynamics
        // K = 180.0 (spring coefficient), D = 22.0 (damping ratio ~ 0.8)
        float springForce = diff * 180.0f;
        float dampingForce = m_velocities[i] * 22.0f;
        float accel = springForce - dampingForce;

        m_velocities[i] += accel * dt;
        m_currentAngles[i] += m_velocities[i] * dt;

        // Dial physical limits (allow a bit of overshoot beyond -35 and +35)
        m_currentAngles[i] = std::clamp(m_currentAngles[i], -45.0f, 45.0f);

        if (std::abs(m_velocities[i]) > 0.01f || std::abs(diff) > 0.01f) {
            needsUpdate = true;
        }
    }

    if (needsUpdate) {
        update();
    }
}
