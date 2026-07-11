#include "ui/LevelMeterView.h"

#include "models/MonitoringController.h"
#include "ui/StyleTheme.h"

#include <QHBoxLayout>
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

void LevelMeterView::setLevels(const std::vector<float>& rms, const std::vector<float>& peak, const QString& title) {
    m_rms = rms;
    m_peak = peak;
    m_title = title;

    if (m_peakHold.size() != m_peak.size()) {
        m_peakHold.resize(m_peak.size(), 0.0f);
        for (size_t i = 0; i < m_peak.size(); ++i) {
            m_peakHold[i] = normDB(m_peak[i]);
        }
    } else {
        for (size_t i = 0; i < m_peak.size(); ++i) {
            float normP = normDB(m_peak[i]);
            if (normP >= m_peakHold[i]) {
                m_peakHold[i] = normP;
            } else {
                m_peakHold[i] = std::max(0.0f, m_peakHold[i] * 0.95f);
            }
        }
    }

    update();
}

void LevelMeterView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    int w = width();
    int h = height();

    p.setFont(QFont("sans-serif", 11, QFont::Bold));
    p.setPen(StyleTheme::textSecondary());
    p.drawText(16, 24, m_title);

    size_t chCount = std::max(m_rms.size(), m_peak.size());
    if (chCount == 0)
        chCount = 2; // Default 2 channels

    int barAreaTop = 40;
    int barAreaHeight = h - 60;
    int barHeight = (barAreaHeight - static_cast<int>(chCount - 1) * 12) / static_cast<int>(chCount);
    barHeight = std::max(16, std::min(28, barHeight));

    for (size_t i = 0; i < chCount; ++i) {
        int y = barAreaTop + static_cast<int>(i) * (barHeight + 12);
        int xStart = 80;
        int rightMargin = 120;
        int barW = w - xStart - rightMargin;

        QString chLabel = (i == 0) ? "LEFT" : (i == 1 ? "RIGHT" : QString("CH %1").arg(i + 1));
        p.setFont(QFont("monospace", 10, QFont::Bold));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(16, y + barHeight / 2 + 4, chLabel);

        // Background track
        p.fillRect(xStart, y, barW, barHeight, StyleTheme::isDark() ? QColor("#16161a") : QColor("#e5e5ea"));

        // Tick Marks
        p.setPen(QPen(StyleTheme::gridPenColor(), 1));
        for (int dbMark : {-48, -36, -24, -12, -6, -3, 0}) {
            int pos = xStart + static_cast<int>(barW * normDB(static_cast<float>(dbMark)));
            p.drawLine(pos, y, pos, y + barHeight);
        }

        float rmsVal = (i < m_rms.size()) ? m_rms[i] : -100.0f;
        float peakVal = (i < m_peak.size()) ? m_peak[i] : -100.0f;

        float rmsFrac = normDB(rmsVal);
        float peakFrac = (i < m_peakHold.size()) ? m_peakHold[i] : normDB(peakVal);

        int rmsW = static_cast<int>(rmsFrac * barW);
        int peakX = xStart + static_cast<int>(peakFrac * barW);

        int halfH = std::max(4, barHeight / 2);
        // RMS fill (top half)
        QLinearGradient grad(xStart, y, xStart + barW, y);
        grad.setColorAt(0.0, QColor(52, 199, 89, 230));
        grad.setColorAt(0.35, QColor(52, 199, 89, 230));
        grad.setColorAt(0.55, QColor(255, 204, 0, 230));
        grad.setColorAt(0.75, QColor(255, 149, 0, 230));
        grad.setColorAt(0.95, QColor(255, 59, 48, 230));
        grad.setColorAt(1.0, QColor(255, 59, 48, 230));
        p.fillRect(xStart, y, rmsW, halfH - 1, grad);

        // Peak fill (bottom half)
        int peakW = static_cast<int>(peakFrac * barW);
        p.fillRect(xStart, y + halfH + 1, peakW, halfH - 1, grad);

        // Peak line indicator
        if (peakFrac > 0) {
            p.setPen(QPen(StyleTheme::isDark() ? QColor("#ffffff") : QColor("#1a1a1a"), 2));
            p.drawLine(peakX, y, peakX, y + barHeight);
        }

        // Monospace numeric dBFS readouts
        p.setFont(QFont("monospace", 9, QFont::Medium));
        p.setPen(StyleTheme::textSecondary());
        QString rmsTxt = QString("RMS: %1").arg(rmsVal < -90 ? "  -inf" : QString("%1 dB").arg(rmsVal, 5, 'f', 1));
        QString peakTxt = QString("PK: %1").arg(peakVal < -90 ? "  -inf" : QString("%1 dB").arg(peakVal, 5, 'f', 1));
        p.drawText(xStart + barW + 10, y + barHeight / 2 - 2, rmsTxt);
        p.drawText(xStart + barW + 10, y + barHeight - 2, peakTxt);
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

static QColor getLevelColor(float normVal) {
    if (normVal < 0.35f)
        return QColor("#34c759"); // Green
    if (normVal < 0.55f)
        return QColor("#ffcc00"); // Yellow
    if (normVal < 0.75f)
        return QColor("#ff9500"); // Orange
    return QColor("#ff3b30");     // Red
}

void CompactLevelMeterBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    const auto& st = m_monitoring->levelState;

    size_t capCount = st.captureRms.size();
    size_t pbCount = st.playbackRms.size();

    size_t effectiveCap = (capCount > 0) ? std::min(capCount, (size_t)8) : 2;
    size_t effectivePb = (pbCount > 0) ? std::min(pbCount, (size_t)8) : 2;

    int statusAreaWidth = 160;
    int reservedText = 130;
    int availableWidth = std::max(100, w - statusAreaWidth - reservedText - 32);

    size_t totalBars = effectiveCap + effectivePb;
    int barW = std::clamp(availableWidth / static_cast<int>(totalBars), 24, 90);
    int barH = 8;
    int yPos = (height() - barH) / 2;

    int startX = 12;

    QColor trackBg = StyleTheme::isDark() ? QColor(255, 255, 255, 25) : QColor(0, 0, 0, 25);

    // Draw Capture Meters (Mic icon + bars)
    p.setFont(QFont("sans-serif", 9, QFont::Bold));
    p.setPen(StyleTheme::textSecondary());
    p.drawText(startX, yPos + barH + 1, "🎤 IN:");
    startX += 45;

    for (size_t i = 0; i < effectiveCap; ++i) {
        p.fillRect(startX, yPos, barW, barH, trackBg);
        if (i < st.capturePeak.size()) {
            float val = st.capturePeak[i];
            float frac = normDB(val);
            int fillW = static_cast<int>(frac * barW);
            if (fillW > 0) {
                p.fillRect(startX, yPos, fillW, barH, getLevelColor(frac));
            }
        }
        startX += barW + 4;
    }

    startX += 12;
    // Draw Playback Meters (Speaker icon + bars)
    p.drawText(startX, yPos + barH + 1, "🔊 OUT:");
    startX += 55;

    for (size_t i = 0; i < effectivePb; ++i) {
        p.fillRect(startX, yPos, barW, barH, trackBg);
        if (i < st.playbackPeak.size()) {
            float val = st.playbackPeak[i];
            float frac = normDB(val);
            int fillW = static_cast<int>(frac * barW);
            if (fillW > 0) {
                p.fillRect(startX, yPos, fillW, barH, getLevelColor(frac));
            }
        }
        startX += barW + 4;
    }
}
