#include "ui/AnalogVUMeterView.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainterPath>
#include <QRadialGradient>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// MARK: - Single AnalogVUMeter Implementation

AnalogVUMeter::AnalogVUMeter(int channelIndex, const VUSettings& settings, QWidget* parent)
    : QWidget(parent), m_channelIndex(channelIndex), m_settings(settings) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(80, 80);
}

void AnalogVUMeter::setLevel(float dbFS) {
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

QSize AnalogVUMeter::sizeHint() const {
    return QSize(220, 160);
}

QSize AnalogVUMeter::minimumSizeHint() const {
    return QSize(80, 80);
}

float AnalogVUMeter::computeAngleForLevel(float dbFS) const {
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
    qreal dpr = devicePixelRatioF();
    pixmap = QPixmap(size * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);

    QRect dialRect(0, 0, size.width(), size.height());
    double w = dialRect.width();
    double h = dialRect.height();

    QColor bulbAmberColor, bulbHotSpotColor, arcColor, percentageMarksColor, redZoneColor;
    QColor textCol = palette().color(QPalette::Text);
    QColor subtextCol = palette().color(QPalette::PlaceholderText);

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
    p.fillRect(dialRect, palette().color(QPalette::Base));

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
    QPainterPath boxPath;
    boxPath.addRoundedRect(dialRect, 6 * scale, 6 * scale);
    p.setPen(QPen(palette().color(QPalette::Mid), 1.2 * scale));
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

    QRect dialRect(innerRect.left(), innerRect.top(), innerRect.width(), innerRect.height() - labelHeight - vSpacing);
    QRect labelRect(innerRect.left(), dialRect.bottom() + vSpacing, innerRect.width(), labelHeight);

    if (m_cachedDialPixmap.isNull() || m_cachedDialSize != dialRect.size() ||
        std::abs(m_cachedScale - scale) > 0.001f || m_cachedTheme != m_settings.theme) {
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
    QPainterPath clipPath;
    clipPath.addRoundedRect(dialRect, 6 * scale, 6 * scale);
    p.setClipPath(clipPath);

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
    QColor lblColor = palette().color(QPalette::PlaceholderText);
    p.setPen(lblColor);
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
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_canvasWidget = new QWidget(m_scrollArea);
    m_canvasLayout = new QHBoxLayout(m_canvasWidget);
    m_canvasLayout->setContentsMargins(0, 0, 0, 0);
    m_canvasLayout->setSpacing(16);

    m_scrollArea->setWidget(m_canvasWidget);
    rootLayout->addWidget(m_scrollArea);

    updateChannelMeters();
}

AnalogVUMeterView::~AnalogVUMeterView() {
    if (m_levelState && m_levelState->visibilityCount > 0) {
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
    if (m_levelState && !m_levelState->playbackRms.empty()) {
        levels = m_levelState->playbackRms;
    } else if (!m_levels.empty()) {
        levels = m_levels;
    } else {
        levels = {-100.0f, -100.0f};
    }

    size_t count = std::max(static_cast<size_t>(1), levels.size());

    // Adjust meter widget count
    while (m_meters.size() < count) {
        int idx = static_cast<int>(m_meters.size());
        auto* meter = new AnalogVUMeter(idx, m_settings, m_canvasWidget);
        meter->setGainCalibration(m_gainCalibrationDb);
        m_canvasLayout->addWidget(meter);
        m_meters.push_back(meter);
    }
    while (m_meters.size() > count) {
        auto* meter = m_meters.back();
        m_meters.pop_back();
        m_canvasLayout->removeWidget(meter);
        delete meter;
    }

    // Set sizing policies matching SwiftUI
    if (count <= 4) {
        m_canvasLayout->setAlignment(Qt::Alignment());
        for (size_t i = 0; i < count; ++i) {
            m_meters[i]->setMinimumWidth(0);
            m_meters[i]->setMaximumWidth(QWIDGETSIZE_MAX);
            m_meters[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            m_meters[i]->setLevel(levels[i]);
            m_meters[i]->setChannelIndex(static_cast<int>(i));
        }
    } else {
        m_canvasLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (size_t i = 0; i < count; ++i) {
            m_meters[i]->setFixedWidth(220);
            m_meters[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
            m_meters[i]->setLevel(levels[i]);
            m_meters[i]->setChannelIndex(static_cast<int>(i));
        }
    }
}
