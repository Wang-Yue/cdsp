#include "ui/LevelMeterView.h"

#include "models/MonitoringController.h"

#include <QHBoxLayout>
#include <QPainterPath>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

LevelMeterView::LevelMeterView(QWidget* parent) : QWidget(parent) {}

LevelMeterView::~LevelMeterView() {
    if (isVisible() && m_levelState && m_levelState->visibilityCount > 0) {
        m_levelState->visibilityCount--;
    }
}

void LevelMeterView::setLevelState(LevelState* levelState) {
    if (m_levelState == levelState)
        return;
    if (isVisible() && m_levelState && m_levelState->visibilityCount > 0)
        m_levelState->visibilityCount--;
    m_levelState = levelState;
    if (isVisible() && m_levelState)
        m_levelState->visibilityCount++;
    update();
}

QSize LevelMeterView::sizeHint() const {
    size_t chCount = 2;
    if (m_hasExplicitLevels) {
        chCount = m_rms.size();
    } else if (m_levelState) {
        std::lock_guard<std::mutex> lock(m_levelState->mutex);
        chCount = m_isCapture ? m_levelState->captureRms.size() : m_levelState->playbackRms.size();
    }
    if (chCount == 0)
        chCount = 2;    // Default to 2 channels
    int barHeight = 18; // Match SwiftUI height: 18px per channel
    int spacing = 8;
    int basePadding = m_title.isEmpty() ? 8 : 50;
    int totalH = static_cast<int>(chCount) * barHeight + static_cast<int>(chCount - 1) * spacing + basePadding;
    return QSize(300, totalH);
}

QSize LevelMeterView::minimumSizeHint() const {
    QSize sh = sizeHint();
    return QSize(180, sh.height());
}

void LevelMeterView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_levelState)
        m_levelState->visibilityCount++;
}

void LevelMeterView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_levelState && m_levelState->visibilityCount > 0)
        m_levelState->visibilityCount--;
}

static float normDB(float db) {
    if (std::isnan(db) || db < -60.0f)
        return 0.0f;
    if (db > 0.0f)
        return 1.0f;
    return (db + 60.0f) / 60.0f;
}

static QColor appThemeColor(float value) {
    float v = std::clamp(value, 0.0f, 1.0f);
    if (v < 0.35f) {
        return QColor(0, 255, 0); // green
    } else if (v < 0.55f) {
        float t = (v - 0.35f) / 0.2f;
        return QColor(static_cast<int>(255 * t), 255, 0);
    } else if (v < 0.75f) {
        float t = (v - 0.55f) / 0.2f;
        return QColor(255, static_cast<int>(255 * (1.0f - t * 0.5f)), 0);
    } else if (v < 0.95f) {
        float t = (v - 0.75f) / 0.2f;
        return QColor(255, static_cast<int>(128 * (1.0f - t)), 0);
    } else {
        return QColor(255, 0, 0); // red
    }
}

void LevelMeterView::setLevels(const std::vector<float>& rms, const std::vector<float>& peak, const QString& title) {
    bool sizeChanged = (m_rms.size() != rms.size() || m_title != title);
    m_rms = rms;
    m_peak = peak;
    m_title = title;
    m_hasExplicitLevels = true;
    if (sizeChanged) {
        updateGeometry();
    }
    update();
}

void LevelMeterView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    bool inMiniPlayer = (parentWidget() && parentWidget()->inherits("QStackedWidget"));
    if (!inMiniPlayer) {
        p.fillRect(rect(), palette().color(QPalette::Base));
    }

    int w = width();
    int h = height();

    QColor subtextColor = palette().color(QPalette::PlaceholderText);
    QColor midColor = palette().color(QPalette::Mid);

    if (!m_title.isEmpty()) {
        QFont titleF = font();
        titleF.setPointSize(11);
        titleF.setBold(true);
        p.setFont(titleF);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(16, 24, m_title);
    }

    std::vector<float> rmsVec;
    std::vector<float> peakVec;
    if (m_hasExplicitLevels) {
        rmsVec = m_rms;
        peakVec = m_peak;
    } else if (m_levelState) {
        std::lock_guard<std::mutex> lock(m_levelState->mutex);
        rmsVec = m_isCapture ? m_levelState->captureRms : m_levelState->playbackRms;
        peakVec = m_isCapture ? m_levelState->capturePeak : m_levelState->playbackPeak;
    }

    size_t chCount = std::max(rmsVec.size(), peakVec.size());
    if (chCount == 0)
        chCount = 2; // Default 2 channels

    int barAreaTop = m_title.isEmpty() ? 4 : 36;
    int barAreaHeight = h - (m_title.isEmpty() ? 8 : 50);
    int barHeight = (barAreaHeight - static_cast<int>(chCount - 1) * 8) / static_cast<int>(chCount);
    barHeight = std::clamp(barHeight, 16, 24);

    for (size_t i = 0; i < chCount; ++i) {
        int y = barAreaTop + static_cast<int>(i) * (barHeight + 8);
        int labelW = 14;
        int rightMargin = 44;
        int xStart = 28;
        int barW = w - xStart - rightMargin - 12;
        if (barW < 10)
            continue;

        // 1. Channel Label ("1", "2", "3"...)
        QString chLabel = QString::number(i + 1);

        QFont chFont("monospace", 10, QFont::Medium);
        chFont.setStyleHint(QFont::Monospace);
        p.setFont(chFont);
        p.setPen(subtextColor);
        p.drawText(0, y, labelW + 10, barHeight, Qt::AlignCenter, chLabel);

        // 2. Track Background with Corner Radius = 3px
        QPainterPath trackPath;
        trackPath.addRoundedRect(QRectF(xStart, y, barW, barHeight), 3, 3);
        QColor trackBg = palette().color(QPalette::Dark);
        p.fillPath(trackPath, trackBg);

        // 3. Horizontal Center Divider
        int halfH = barHeight / 2;
        QColor dividerColor = palette().color(QPalette::Mid);
        p.setPen(QPen(dividerColor, 0.5));
        p.drawLine(xStart, y + halfH, xStart + barW, y + halfH);

        // 4. Tick Marks (-48 to 0 dB)
        QColor tickColor = palette().color(QPalette::Midlight);
        p.setPen(QPen(tickColor, 1));
        for (int dbMark : {-48, -36, -24, -12, -6, -3, 0}) {
            int pos = xStart + static_cast<int>(barW * normDB(static_cast<float>(dbMark)));
            int markH = (dbMark == 0) ? barHeight : (barHeight / 2);
            int markY = (dbMark == 0) ? y : (y + (barHeight - markH) / 2);
            p.drawLine(pos, markY, pos, markY + markH);
        }

        float rmsVal = (i < rmsVec.size()) ? rmsVec[i] : -100.0f;
        float peakVal = (i < peakVec.size()) ? peakVec[i] : -100.0f;
        if (std::isnan(rmsVal))
            rmsVal = -100.0f;
        if (std::isnan(peakVal))
            peakVal = -100.0f;

        float rmsFrac = normDB(rmsVal);
        float peakFrac = normDB(peakVal);

        int rmsW = static_cast<int>(rmsFrac * barW);
        int peakW = static_cast<int>(peakFrac * barW);

        // Level Linear Gradient (Audio Level: green -> yellow -> orange -> red with 0.9 opacity)
        QLinearGradient grad(xStart, y, xStart + barW, y);
        grad.setColorAt(0.00, QColor(0, 255, 0, 230));
        grad.setColorAt(0.35, QColor(0, 255, 0, 230));
        grad.setColorAt(0.55, QColor(255, 255, 0, 230));
        grad.setColorAt(0.75, QColor(255, 128, 0, 230));
        grad.setColorAt(0.95, QColor(255, 0, 0, 230));
        grad.setColorAt(1.00, QColor(255, 0, 0, 230));

        // 5. RMS Bar (Top Half, Corner Radius = 2px)
        if (rmsW > 0) {
            QPainterPath rmsPath;
            rmsPath.addRoundedRect(QRectF(xStart, y + 0.5, rmsW, halfH - 1), 2, 2);
            p.fillPath(rmsPath, grad);
        }

        // 6. Peak Bar (Bottom Half, Corner Radius = 2px)
        if (peakW > 0) {
            QPainterPath peakPath;
            peakPath.addRoundedRect(QRectF(xStart, y + halfH + 0.5, peakW, halfH - 1), 2, 2);
            p.fillPath(peakPath, grad);
        }

        // 7. Stacked Monospace Numeric Readouts (%5.1f format)
        QFont monoFont("monospace", 9, QFont::Normal);
        monoFont.setStyleHint(QFont::Monospace);
        p.setFont(monoFont);
        QString rmsStr = QString::asprintf("%5.1f", rmsVal);
        QString peakStr = QString::asprintf("%5.1f", peakVal);

        p.setPen(palette().color(QPalette::Text));
        p.drawText(xStart + barW + 4, y, rightMargin, halfH, Qt::AlignRight | Qt::AlignVCenter, rmsStr);

        p.setPen(subtextColor);
        p.drawText(xStart + barW + 4, y + halfH, rightMargin, halfH, Qt::AlignRight | Qt::AlignVCenter, peakStr);
    }
}

static void drawMicIcon(QPainter& p, int x, int y, const QPalette& pal) {
    p.save();
    p.translate(x + 6, y + 6); // center inside 12x12
    p.setPen(QPen(pal.color(QPalette::Text), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    QPainterPath mic;
    mic.addRoundedRect(QRectF(-2.5, -5, 5, 8), 2.5, 2.5);
    p.drawPath(mic);

    QPainterPath stand;
    stand.arcMoveTo(QRectF(-4.5, -2, 9, 8), -180);
    stand.arcTo(QRectF(-4.5, -2, 9, 8), -180, 180);
    stand.moveTo(0, 6);
    stand.lineTo(0, 9);
    stand.moveTo(-3, 9);
    stand.lineTo(3, 9);
    p.drawPath(stand);
    p.restore();
}

static void drawSpeakerIcon(QPainter& p, int x, int y, const QPalette& pal) {
    p.save();
    p.translate(x + 6, y + 6); // center inside 12x12
    p.setPen(QPen(pal.color(QPalette::Text), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    // Outer box
    p.drawRoundedRect(QRectF(-4.5, -6, 9, 12), 1, 1);
    // Tweeter
    p.drawEllipse(QRectF(-1.5, -4, 3, 3));
    // Woofer
    p.drawEllipse(QRectF(-2.5, 0, 5, 5));
    p.restore();
}

class CompactMultiChannelMeter : public QWidget {
public:
    explicit CompactMultiChannelMeter(bool isPlayback, QWidget* parent = nullptr)
        : QWidget(parent), m_isPlayback(isPlayback) {
        setFixedHeight(6);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    void setLevelState(LevelState* levelState) {
        if (m_levelState == levelState)
            return;
        m_levelState = levelState;
        updateLayoutAndGeometry();
        update();
    }
    size_t getCount() const {
        if (!m_levelState)
            return 2;
        std::lock_guard<std::mutex> lock(m_levelState->mutex);
        size_t count = m_isPlayback ? m_levelState->playbackPeak.size() : m_levelState->capturePeak.size();
        if (count == 0)
            count = m_isPlayback ? m_levelState->playbackChannelCount : m_levelState->captureChannelCount;
        return (count > 0) ? count : 2;
    }
    void updateLayoutAndGeometry() {
        size_t count = getCount();
        if (count == m_lastCount && width() > 0)
            return;
        m_lastCount = count;
        int barW = (count > 4) ? 40 : 80;
        int spacing = 4;
        int totalWidth = (count > 0) ? static_cast<int>((barW + spacing) * count - spacing) : 0;
        setFixedWidth(totalWidth);
        updateGeometry();
    }
    QSize sizeHint() const override {
        size_t count = getCount();
        int barW = (count > 4) ? 40 : 80;
        int spacing = 4;
        int totalWidth = (count > 0) ? static_cast<int>((barW + spacing) * count - spacing) : 0;
        return QSize(totalWidth, 6);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        if (!m_levelState)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        std::vector<float> peakLevels;
        {
            std::lock_guard<std::mutex> lock(m_levelState->mutex);
            peakLevels = m_isPlayback ? m_levelState->playbackPeak : m_levelState->capturePeak;
        }

        size_t count = getCount();
        if (count == 0)
            return;

        int barW = (count > 4) ? 40 : 80;
        int barH = 6;
        int spacing = 4;
        QColor trackBg = palette().color(QPalette::Mid);

        for (size_t i = 0; i < count; ++i) {
            int x = static_cast<int>(i * (barW + spacing));
            QPainterPath trackPath;
            trackPath.addRoundedRect(QRectF(x, 0, barW, barH), 1.5, 1.5);
            p.fillPath(trackPath, trackBg);

            if (i < peakLevels.size()) {
                float frac = normDB(peakLevels[i]);
                int fillW = static_cast<int>(frac * barW);
                if (fillW > 0) {
                    QPainterPath fillPath;
                    fillPath.addRoundedRect(QRectF(x, 0, fillW, barH), 1.5, 1.5);
                    p.fillPath(fillPath, appThemeColor(frac));
                }
            }
        }
    }

private:
    bool m_isPlayback;
    LevelState* m_levelState = nullptr;
    size_t m_lastCount = 0;
};

class MeterGroupWidget : public QWidget {
public:
    MeterGroupWidget(bool isPlayback, LevelState* levelState, QWidget* parent = nullptr)
        : QWidget(parent), m_isPlayback(isPlayback) {
        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        // 14px space on left for icon
        auto spacer = new QWidget(this);
        spacer->setFixedWidth(14);
        layout->addWidget(spacer);

        m_meter = new CompactMultiChannelMeter(isPlayback, this);
        m_meter->setLevelState(levelState);
        layout->addWidget(m_meter);

        setFixedHeight(16);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        updateWidth();
    }
    void setLevelState(LevelState* levelState) {
        m_meter->setLevelState(levelState);
        updateWidth();
    }
    void updateWidth() {
        if (m_meter) {
            int oldW = m_meter->width();
            m_meter->updateLayoutAndGeometry();
            if (m_meter->width() != oldW || width() == 0) {
                int totalW = 14 + 6 + m_meter->width();
                setFixedWidth(totalW);
                updateGeometry();
            }
        }
    }
    void updateMeters() {
        updateWidth();
        update();
        if (m_meter)
            m_meter->update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (m_isPlayback) {
            drawSpeakerIcon(p, 0, (height() - 12) / 2, palette());
        } else {
            drawMicIcon(p, 0, (height() - 12) / 2, palette());
        }
    }

private:
    bool m_isPlayback;
    CompactMultiChannelMeter* m_meter;
};

// MARK: - CompactLevelMeterBar Implementation

CompactLevelMeterBar::CompactLevelMeterBar(std::shared_ptr<MonitoringController> monitoring,
                                           std::shared_ptr<DSPEngineController> dsp, QWidget* parent)
    : QWidget(parent), m_dsp(dsp) {
    setFixedHeight(20);

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(18);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }\n"
                          "QScrollArea > QWidget > QWidget { background: transparent; border: none; }");
    scroll->viewport()->setAutoFillBackground(false);
    scroll->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);

    auto container = new QWidget(scroll);
    container->setAutoFillBackground(false);
    container->setAttribute(Qt::WA_TranslucentBackground, true);
    auto containerLayout = new QHBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(12);
    containerLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_captureGroup = new MeterGroupWidget(false, nullptr, container);
    m_playbackGroup = new MeterGroupWidget(true, nullptr, container);

    containerLayout->addWidget(m_captureGroup);
    containerLayout->addWidget(m_playbackGroup);
    containerLayout->addStretch();

    scroll->setWidget(container);
    layout->addWidget(scroll, 1);

    setMonitoring(monitoring);
}

CompactLevelMeterBar::~CompactLevelMeterBar() {
    if (isVisible() && m_monitoring && m_monitoring->levelState.visibilityCount > 0) {
        m_monitoring->levelState.visibilityCount--;
    }
}

void CompactLevelMeterBar::setMonitoring(std::shared_ptr<MonitoringController> monitoring) {
    if (m_monitoring == monitoring)
        return;
    if (m_monitoring) {
        if (isVisible() && m_monitoring->levelState.visibilityCount > 0)
            m_monitoring->levelState.visibilityCount--;
        disconnect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, nullptr);
    }
    m_monitoring = monitoring;
    LevelState* levelState = m_monitoring ? &m_monitoring->levelState : nullptr;
    if (m_captureGroup)
        m_captureGroup->setLevelState(levelState);
    if (m_playbackGroup)
        m_playbackGroup->setLevelState(levelState);

    if (m_monitoring) {
        if (isVisible())
            m_monitoring->levelState.visibilityCount++;
        connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, [this]() {
            if (m_captureGroup)
                m_captureGroup->updateMeters();
            if (m_playbackGroup)
                m_playbackGroup->updateMeters();
        });
    }
}

void CompactLevelMeterBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_monitoring)
        m_monitoring->levelState.visibilityCount++;
}

void CompactLevelMeterBar::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_monitoring && m_monitoring->levelState.visibilityCount > 0)
        m_monitoring->levelState.visibilityCount--;
}
