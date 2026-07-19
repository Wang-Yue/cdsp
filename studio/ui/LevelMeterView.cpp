#include "ui/LevelMeterView.h"

#include "models/MonitoringController.h"
#include "ui/StyleTheme.h"

#include <QHBoxLayout>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

LevelMeterView::LevelMeterView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);
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
    if (db < -60.0f)
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
    m_rms = rms;
    m_peak = peak;
    m_title = title;
    update();
}

void LevelMeterView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (!parentWidget() || !parentWidget()->inherits("QStackedWidget")) {
        p.fillRect(rect(), StyleTheme::cardBg());
    }

    int w = width();
    int h = height();

    p.setFont(QFont("sans-serif", 11, QFont::Bold));
    p.setPen(StyleTheme::textSecondary());
    p.drawText(16, 24, m_title);

    const std::vector<float>& rmsVec =
        (!m_rms.empty()) ? m_rms : (m_levelState ? m_levelState->playbackRms : std::vector<float>{});
    const std::vector<float>& peakVec =
        (!m_peak.empty()) ? m_peak : (m_levelState ? m_levelState->playbackPeak : std::vector<float>{});

    size_t chCount = std::max(rmsVec.size(), peakVec.size());
    if (chCount == 0)
        chCount = 2; // Default 2 channels

    int barAreaTop = 36;
    int barAreaHeight = h - 50;
    int barHeight = (barAreaHeight - static_cast<int>(chCount - 1) * 8) / static_cast<int>(chCount);
    barHeight = std::clamp(barHeight, 16, 24);

    for (size_t i = 0; i < chCount; ++i) {
        int y = barAreaTop + static_cast<int>(i) * (barHeight + 8);
        int labelW = 14;
        int rightMargin = 44;
        int xStart = 28;
        int barW = w - xStart - rightMargin - 12;

        // 1. Channel Label ("1", "2", "3"...)
        QString chLabel = QString::number(i + 1);

        p.setFont(QFont("monospace", 10, QFont::Medium));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(0, y, labelW + 10, barHeight, Qt::AlignCenter, chLabel);

        // 2. Track Background with Corner Radius = 3px
        QPainterPath trackPath;
        trackPath.addRoundedRect(QRectF(xStart, y, barW, barHeight), 3, 3);
        QColor trackBg = StyleTheme::isDark() ? QColor(255, 255, 255, 15) : QColor(0, 0, 0, 15);
        p.fillPath(trackPath, trackBg);

        // 3. Horizontal Center Divider
        int halfH = barHeight / 2;
        p.setPen(QPen(StyleTheme::isDark() ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 20), 0.5));
        p.drawLine(xStart, y + halfH, xStart + barW, y + halfH);

        // 4. Tick Marks (-48 to 0 dB)
        p.setPen(QPen(StyleTheme::isDark() ? QColor(255, 255, 255, 50) : QColor(0, 0, 0, 50), 1));
        for (int dbMark : {-48, -36, -24, -12, -6, -3, 0}) {
            int pos = xStart + static_cast<int>(barW * normDB(static_cast<float>(dbMark)));
            int markH = (dbMark == 0) ? barHeight : (barHeight / 2);
            int markY = (dbMark == 0) ? y : (y + (barHeight - markH) / 2);
            p.drawLine(pos, markY, pos, markY + markH);
        }

        float rmsVal = (i < rmsVec.size()) ? rmsVec[i] : -100.0f;
        float peakVal = (i < peakVec.size()) ? peakVec[i] : -100.0f;

        float rmsFrac = normDB(rmsVal);
        float peakFrac = normDB(peakVal);

        int rmsW = static_cast<int>(rmsFrac * barW);
        int peakW = static_cast<int>(peakFrac * barW);

        // Level Linear Gradient
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
        p.setFont(QFont("monospace", 9, QFont::Normal));
        p.setPen(StyleTheme::textSecondary());
        QString rmsStr = QString::asprintf("%5.1f", rmsVal);
        QString peakStr = QString::asprintf("%5.1f", peakVal);

        p.drawText(xStart + barW + 4, y, rightMargin, halfH, Qt::AlignRight | Qt::AlignVCenter, rmsStr);
        p.drawText(xStart + barW + 4, y + halfH, rightMargin, halfH, Qt::AlignRight | Qt::AlignVCenter, peakStr);
    }
}

// MARK: - CompactLevelMeterBar Implementation

CompactLevelMeterBar::CompactLevelMeterBar(std::shared_ptr<MonitoringController> monitoring,
                                           std::shared_ptr<DSPEngineController> dsp, QWidget* parent)
    : QWidget(parent), m_monitoring(monitoring), m_dsp(dsp) {
    setFixedHeight(36);
    setStyleSheet("background-color: transparent;");

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 4, 12, 4);

    m_statusDot = new QWidget(this);
    m_statusDot->setFixedSize(8, 8);
    m_statusDot->setStyleSheet("background-color: #8e8e93; border-radius: 4px;");

    m_statusLabel = new QLabel("Inactive", this);
    m_statusLabel->setFont(QFont("sans-serif", 10, QFont::Bold));
    m_statusLabel->setStyleSheet("color: #8e8e93;");

    layout->addStretch();
    layout->addWidget(m_statusDot);
    layout->addWidget(m_statusLabel);

    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, [this]() { update(); });
    connect(m_dsp.get(), &DSPEngineController::statusChanged, this, [this](ProcessingState) { updateState(); });
    updateState();
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

void CompactLevelMeterBar::updateState() {
    ProcessingState st = m_dsp->status;
    switch (st) {
    case ProcessingState::Running:
        m_statusDot->setStyleSheet("background-color: #34c759; border-radius: 4px;");
        m_statusLabel->setText("Running");
        m_statusLabel->setStyleSheet("color: #34c759;");
        break;
    case ProcessingState::Paused:
        m_statusDot->setStyleSheet("background-color: #007aff; border-radius: 4px;");
        m_statusLabel->setText("Paused");
        m_statusLabel->setStyleSheet("color: #007aff;");
        break;
    case ProcessingState::Stalled:
        m_statusDot->setStyleSheet("background-color: #ff9500; border-radius: 4px;");
        m_statusLabel->setText("Stalled");
        m_statusLabel->setStyleSheet("color: #ff9500;");
        break;
    case ProcessingState::Starting:
        m_statusDot->setStyleSheet("background-color: #ffcc00; border-radius: 4px;");
        m_statusLabel->setText("Starting...");
        m_statusLabel->setStyleSheet("color: #ffcc00;");
        break;
    case ProcessingState::Inactive:
    default:
        m_statusDot->setStyleSheet("background-color: #8e8e93; border-radius: 4px;");
        m_statusLabel->setText("Inactive");
        m_statusLabel->setStyleSheet("color: #8e8e93;");
        break;
    }
}

void CompactLevelMeterBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!m_monitoring)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& st = m_monitoring->levelState;
    size_t capCount = st.captureRms.size();
    size_t pbCount = st.playbackRms.size();

    size_t effectiveCap = (capCount > 0) ? capCount : 2;
    size_t effectivePb = (pbCount > 0) ? pbCount : 2;

    int barW = (effectiveCap > 4) ? 40 : 80;
    int barH = 6;
    int spacing = 4;
    int yPos = (height() - barH) / 2;
    int startX = 12;

    QColor trackBg = StyleTheme::isDark() ? QColor(255, 255, 255, 15) : QColor(0, 0, 0, 15);

    // Draw Capture Level Bars
    for (size_t i = 0; i < effectiveCap; ++i) {
        QPainterPath trackPath;
        trackPath.addRoundedRect(QRectF(startX, yPos, barW, barH), 1.5, 1.5);
        p.fillPath(trackPath, trackBg);

        if (i < st.capturePeak.size()) {
            float frac = normDB(st.capturePeak[i]);
            int fillW = static_cast<int>(frac * barW);
            if (fillW > 0) {
                QPainterPath fillPath;
                fillPath.addRoundedRect(QRectF(startX, yPos, fillW, barH), 1.5, 1.5);
                p.fillPath(fillPath, appThemeColor(frac));
            }
        }
        startX += barW + spacing;
    }

    startX += 16; // Section spacing

    // Draw Playback Level Bars
    for (size_t i = 0; i < effectivePb; ++i) {
        QPainterPath trackPath;
        trackPath.addRoundedRect(QRectF(startX, yPos, barW, barH), 1.5, 1.5);
        p.fillPath(trackPath, trackBg);

        if (i < st.playbackPeak.size()) {
            float frac = normDB(st.playbackPeak[i]);
            int fillW = static_cast<int>(frac * barW);
            if (fillW > 0) {
                QPainterPath fillPath;
                fillPath.addRoundedRect(QRectF(startX, yPos, fillW, barH), 1.5, 1.5);
                p.fillPath(fillPath, appThemeColor(frac));
            }
        }
        startX += barW + spacing;
    }
}
