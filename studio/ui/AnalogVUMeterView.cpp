#include "ui/AnalogVUMeterView.h"

#include "utils/ThemeManager.h" // for ThemeManager

#include <QBrush>           // for QRadialGradient, QBrush, QLinearGradient
#include <QColor>           // for QColor
#include <QEvent>           // for QEvent
#include <QFont>            // for QFont
#include <QFontMetrics>     // for QFontMetrics
#include <QFrame>           // for QFrame
#include <QPainter>         // for QPainter
#include <QPainterPath>     // for QPainterPath
#include <QPalette>         // for QPalette
#include <QPen>             // for QPen
#include <QPointF>          // for QPointF
#include <QRect>            // for QRect
#include <QRectF>           // for QRectF
#include <QSizePolicy>      // for QSizePolicy
#include <QString>          // for QString
#include <QVBoxLayout>      // for QVBoxLayout
#include <Qt>               // for AlignmentFlag, ScrollBarPolicy, Alignment, GlobalColor, operator|
#include <QtGlobal>         // for Q_UNUSED, qFuzzyCompare, qreal
#include <algorithm>        // for max, min
#include <cmath>            // for cos, sin, M_PI, isnan, pow
#include <cstdlib>          // for abs
#include <initializer_list> // for initializer_list
#include <mutex>            // for mutex, lock_guard

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AnalogVUMeter::AnalogVUMeter(int channelIndex, const VUSettings& settings, QWidget* parent)
    : QWidget(parent), m_channelIndex(channelIndex), m_settings(settings) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(80, 80);
}

void AnalogVUMeter::setLevel(float dbFS) {
    if (std::isnan(dbFS))
        dbFS = -100.0f;
    if (std::abs(m_levelDb - dbFS) > 0.01f) {
        m_levelDb = dbFS;
        update();
    }
}

void AnalogVUMeter::setVUSettings(const VUSettings& settings) {
    m_settings = settings;
    m_cachedScale = 0.0f;
    update();
}

void AnalogVUMeter::setGainCalibration(float gainDb) {
    m_gainCalibrationDb = gainDb;
    update();
}

void AnalogVUMeter::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_cachedScale = 0.0f;
}

void AnalogVUMeter::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        m_cachedScale = 0.0f;
        update();
    }
}

QSize AnalogVUMeter::sizeHint() const {
    return QSize(220, 160);
}

QSize AnalogVUMeter::minimumSizeHint() const {
    return QSize(80, 80);
}

float AnalogVUMeter::computeAngleForLevel(float dbFS) const {
    if (std::isnan(dbFS))
        dbFS = -100.0f;
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

void AnalogVUMeter::renderDialBackground(QPixmap& pixmap, const QSize& size, float scale) {
    if (size.width() <= 0 || size.height() <= 0)
        return;

    qreal dpr = devicePixelRatioF();
    pixmap = QPixmap(size * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF dialRect(0, 0, size.width(), size.height());
    double w = dialRect.width();
    double h = dialRect.height();

    bool inMiniPlayer = ThemeManager::isMiniPlayer(this);

    QColor bulbAmberColor, bulbHotSpotColor, arcColor, percentageMarksColor, redZoneColor;
    QColor textCol = inMiniPlayer ? ThemeManager::miniPlayerPrimaryTextColor() : palette().color(QPalette::Text);
    QColor subtextCol =
        inMiniPlayer ? ThemeManager::miniPlayerSubtextColor() : palette().color(QPalette::PlaceholderText);

    if (m_settings.theme == VUTheme::VintageAmber) {
        bulbAmberColor = QColor(255, 209, 102);
        bulbHotSpotColor = QColor(255, 250, 224);
        arcColor = textCol;
        percentageMarksColor = subtextCol;
        redZoneColor = QColor(255, 0, 0);
    } else if (m_settings.theme == VUTheme::DarkStealth) {
        bulbAmberColor = QColor(0, 0, 0);
        bulbHotSpotColor = QColor(255, 255, 255);
        arcColor = textCol;
        percentageMarksColor = subtextCol;
        redZoneColor = QColor(255, 59, 48);
    } else { // Warm Tube
        bulbAmberColor = QColor(242, 115, 26);
        bulbHotSpotColor = QColor(255, 204, 77);
        arcColor = textCol;
        percentageMarksColor = subtextCol;
        redZoneColor = QColor(217, 51, 26);
    }

    QPointF center(w / 2.0, h * m_settings.pivotY);
    double radius = h * m_settings.radiusScale;
    double baseH = h;

    p.save();
    QPainterPath clipPath;
    clipPath.addRoundedRect(dialRect, 6 * scale, 6 * scale);
    p.setClipPath(clipPath);

    if (!inMiniPlayer) {
        p.fillRect(dialRect, palette().color(QPalette::Window));
    }

    // 1. BOTTOM AMBER GLOW
    if (m_settings.ambientGlow > 0) {
        QRadialGradient amberGlow(QPointF(w / 2.0, baseH + 10 * scale), h * 1.6);
        int ambAlpha = static_cast<int>(m_settings.ambientGlow * 255);
        amberGlow.setColorAt(0.0,
                             QColor(bulbAmberColor.red(), bulbAmberColor.green(), bulbAmberColor.blue(), ambAlpha));
        amberGlow.setColorAt(0.8, QColor(bulbAmberColor.red(), bulbAmberColor.green(), bulbAmberColor.blue(), 0));
        amberGlow.setColorAt(1.0, QColor(bulbAmberColor.red(), bulbAmberColor.green(), bulbAmberColor.blue(), 0));
        p.fillRect(dialRect, amberGlow);
    }

    // 2. HOT SPOT
    if (m_settings.hotSpotAlpha > 0) {
        QRadialGradient hotSpot(QPointF(w / 2.0, baseH + 5 * scale), h * 0.4);
        int hsAlpha = static_cast<int>(m_settings.hotSpotAlpha * 255);
        hotSpot.setColorAt(0.0,
                           QColor(bulbHotSpotColor.red(), bulbHotSpotColor.green(), bulbHotSpotColor.blue(), hsAlpha));
        hotSpot.setColorAt(1.0, QColor(bulbHotSpotColor.red(), bulbHotSpotColor.green(), bulbHotSpotColor.blue(), 0));
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
        p.setPen(QPen(color, 1.8 * scale));
        p.drawLine(s, e);

        double lR = radius + 18 * scale;
        QPointF lp(center.x() + cosA * lR, center.y() + sinA * lR);

        p.save();
        p.translate(lp);
        p.rotate(angDeg);
        p.setFont(vintageFont);
        p.setPen(color);
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

        p.setPen(QPen(percentageMarksColor, 1.0 * scale));
        p.drawLine(s, e);

        double lR = radius - 18 * scale;
        QPointF lp(center.x() + cosA * lR, center.y() + sinA * lR);

        p.save();
        p.translate(lp);
        p.rotate(angDeg);
        p.setFont(vintageFont);
        p.setPen(percentageMarksColor);
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
    glassGrad.setColorAt(0.0, QColor(255, 255, 255, 64));
    glassGrad.setColorAt(0.5, QColor(255, 255, 255, 0));
    glassGrad.setColorAt(1.0, QColor(0, 0, 0, 13));
    p.fillRect(dialRect, glassGrad);

    // 7. ADDITIVE LIGHT WASH
    if (m_settings.lightWash > 0) {
        int lwAlpha = static_cast<int>(m_settings.lightWash * 255);
        QColor lwColor(bulbAmberColor.red(), bulbAmberColor.green(), bulbAmberColor.blue(), lwAlpha);
        p.fillRect(dialRect, lwColor);
    }

    p.restore(); // Restore clip

    // Dial Box Outer Border Stroke & Corner Radius
    double strokeW = 1.2 * scale;
    QRectF strokeRect = dialRect.adjusted(strokeW / 2.0, strokeW / 2.0, -strokeW / 2.0, -strokeW / 2.0);
    QPainterPath boxPath;
    boxPath.addRoundedRect(strokeRect, 6 * scale, 6 * scale);
    p.setPen(QPen(palette().color(QPalette::Mid), strokeW));
    p.drawPath(boxPath);
}

void AnalogVUMeter::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    if (w < 20 || h < 20)
        return;

    // Aspect ratio 1.6:1
    constexpr double targetAspect = 1.6;
    double currentAspect = static_cast<double>(w) / static_cast<double>(h);
    int meterW = w;
    int meterH = h;

    if (currentAspect > targetAspect) {
        meterH = h;
        meterW = static_cast<int>(meterH * targetAspect);
    } else {
        meterW = w;
        meterH = static_cast<int>(meterW / targetAspect);
    }

    int meterX = (w - meterW) / 2;
    int meterY = (h - meterH) / 2;
    QRect totalRect(meterX, meterY, meterW, meterH);

    float scale = static_cast<float>(meterH) / 160.0f;

    QRect innerRect = totalRect.adjusted(static_cast<int>(4 * scale), static_cast<int>(4 * scale),
                                         -static_cast<int>(4 * scale), -static_cast<int>(4 * scale));
    int vSpacing = static_cast<int>(6 * scale);

    QFont labelFont("sans-serif", static_cast<int>(std::max(8.0, 11.0 * scale)), QFont::Black);
    QFontMetrics labelFm(labelFont);
    int labelHeight = labelFm.height();

    int dialH = innerRect.height() - labelHeight - vSpacing;
    if (innerRect.width() < 10 || dialH < 10)
        return;

    QRect dialRect(innerRect.left(), innerRect.top(), innerRect.width(), dialH);
    QRect labelRect(innerRect.left(), dialRect.bottom() + vSpacing, innerRect.width(), labelHeight);

    if (m_cachedDialPixmap.isNull() || m_cachedDialSize != dialRect.size() ||
        std::abs(m_cachedScale - scale) > 0.001f || m_cachedTheme != m_settings.theme ||
        !qFuzzyCompare(m_cachedDialPixmap.devicePixelRatioF(), devicePixelRatioF())) {
        renderDialBackground(m_cachedDialPixmap, dialRect.size(), scale);
        m_cachedDialSize = dialRect.size();
        m_cachedScale = scale;
        m_cachedTheme = m_settings.theme;
    }

    // 1. Draw cached dial background
    p.drawPixmap(dialRect.topLeft(), m_cachedDialPixmap);

    // 2. Needle Color
    QColor needleColor = palette().color(QPalette::Text);
    if (m_settings.theme == VUTheme::VintageAmber) {
        // use palette Text
    } else if (m_settings.theme == VUTheme::DarkStealth) {
        needleColor = QColor(255, 255, 255);
    } else {
        needleColor = QColor(38, 38, 38);
    }

    // 3. Draw Needle
    p.save();
    p.setClipRect(dialRect);

    double dw = dialRect.width();
    double dh = dialRect.height();
    QPointF center(dialRect.left() + dw / 2.0, dialRect.top() + dh * m_settings.pivotY);
    double radius = dh * m_settings.radiusScale;
    float angleDeg = computeAngleForLevel(m_levelDb + m_gainCalibrationDb);
    double nAngRad = (angleDeg - 90.0) * M_PI / 180.0;
    double nR = radius + m_settings.needleExtension * scale;
    QPointF ne(center.x() + std::cos(nAngRad) * nR, center.y() + std::sin(nAngRad) * nR);

    p.setPen(QPen(needleColor, 1.2 * scale));
    p.drawLine(center, ne);
    p.restore();

    // 4. Channel Label
    p.setFont(labelFont);
    p.setPen(ThemeManager::subtextColor(this));
    p.drawText(labelRect, Qt::AlignCenter, QString::number(m_channelIndex + 1));
}

// MARK: - AnalogVUMeterView Implementation

AnalogVUMeterView::AnalogVUMeterView(QWidget* parent) : QWidget(parent) {
    auto rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_canvasWidget = new QWidget(m_scrollArea);
    m_canvasLayout = new QHBoxLayout(m_canvasWidget);
    m_canvasLayout->setContentsMargins(0, 0, 0, 0);
    m_canvasLayout->setSpacing(16);

    m_scrollArea->setWidget(m_canvasWidget);
    rootLayout->addWidget(m_scrollArea);

    updateChannelMeters();
}

AnalogVUMeterView::~AnalogVUMeterView() {
    if (isVisible() && m_levelState && m_levelState->visibilityCount > 0) {
        m_levelState->visibilityCount--;
    }
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

void AnalogVUMeterView::setLevelState(LevelState* levelState) {
    if (m_levelState == levelState)
        return;
    if (isVisible() && m_levelState && m_levelState->visibilityCount > 0)
        m_levelState->visibilityCount--;
    m_levelState = levelState;
    if (isVisible() && m_levelState)
        m_levelState->visibilityCount++;
    updateChannelMeters();
}

void AnalogVUMeterView::setLevels(const std::vector<float>& levels) {
    m_levels = levels;
    updateChannelMeters();
}

void AnalogVUMeterView::setLevelDB(float leftDB, float rightDB) {
    setLevels({leftDB, rightDB});
}

void AnalogVUMeterView::setVUSettings(const VUSettings& settings) {
    m_settings = settings;
    for (auto* m : m_meters) {
        m->setVUSettings(settings);
    }
}

void AnalogVUMeterView::setGainCalibration(float gainDb) {
    m_gainCalibrationDb = gainDb;
    for (auto* m : m_meters) {
        m->setGainCalibration(gainDb);
    }
}

void AnalogVUMeterView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        for (auto* m : m_meters) {
            m->update();
        }
    }
}

QSize AnalogVUMeterView::sizeHint() const {
    return QSize(360, 200);
}

QSize AnalogVUMeterView::minimumSizeHint() const {
    return QSize(160, 120);
}

void AnalogVUMeterView::updateChannelMeters() {
    std::vector<float> levels;
    if (m_levelState) {
        std::lock_guard<std::mutex> lock(m_levelState->mutex);
        if (!m_levelState->playbackRms.empty()) {
            levels = m_levelState->playbackRms;
        }
    }
    if (levels.empty() && !m_levels.empty()) {
        levels = m_levels;
    }
    if (levels.empty()) {
        levels = {-100.0f, -100.0f};
    }

    size_t count = std::max(static_cast<size_t>(1), levels.size());
    bool countChanged = (m_currentChannelCount != count || m_meters.size() != count);

    // Adjust meter widget count
    while (m_meters.size() < count) {
        int idx = static_cast<int>(m_meters.size());
        auto* meter = new AnalogVUMeter(idx, m_settings, m_canvasWidget);
        meter->setGainCalibration(m_gainCalibrationDb);
        m_canvasLayout->addWidget(meter);
        m_meters.push_back(meter);
        countChanged = true;
    }
    while (m_meters.size() > count) {
        auto* meter = m_meters.back();
        m_meters.pop_back();
        m_canvasLayout->removeWidget(meter);
        meter->deleteLater();
        countChanged = true;
    }

    // Set sizing policies matching SwiftUI only when channel count changes
    if (countChanged) {
        m_currentChannelCount = count;
        if (count <= 4) {
            m_canvasLayout->setAlignment(Qt::Alignment());
            for (size_t i = 0; i < count; ++i) {
                m_meters[i]->setMinimumWidth(0);
                m_meters[i]->setMaximumWidth(QWIDGETSIZE_MAX);
                m_meters[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
                m_meters[i]->setChannelIndex(static_cast<int>(i));
            }
        } else {
            m_canvasLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            for (size_t i = 0; i < count; ++i) {
                m_meters[i]->setFixedWidth(220);
                m_meters[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
                m_meters[i]->setChannelIndex(static_cast<int>(i));
            }
        }
    }

    // Fast path: update levels on existing meters without layout invalidation
    for (size_t i = 0; i < count; ++i) {
        m_meters[i]->setLevel(levels[i]);
    }
}
