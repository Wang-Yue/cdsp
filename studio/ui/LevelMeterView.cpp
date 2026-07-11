#include "ui/LevelMeterView.h"
#include "ui/StyleTheme.h"
#include "models/MonitoringController.h"
#include <cmath>
#include <algorithm>
#include <QHBoxLayout>
#include <QVBoxLayout>

LevelMeterView::LevelMeterView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);
}

void LevelMeterView::setLevels(const std::vector<float>& rms, const std::vector<float>& peak, const QString& title) {
    m_rms = rms;
    m_peak = peak;
    m_title = title;
    update();
}

static float normDB(float db) {
    if (db < -60.0f) return 0.0f;
    if (db > 0.0f) return 1.0f;
    return (db + 60.0f) / 60.0f;
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
    if (chCount == 0) chCount = 2; // Default 2 channels

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
        p.fillRect(xStart, y, barW, barHeight, QColor("#16161a"));

        // Tick Marks
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        for (int dbMark : {-48, -36, -24, -12, -6, -3, 0}) {
            int pos = xStart + static_cast<int>(barW * normDB(static_cast<float>(dbMark)));
            p.drawLine(pos, y, pos, y + barHeight);
        }

        float rmsVal = (i < m_rms.size()) ? m_rms[i] : -100.0f;
        float peakVal = (i < m_peak.size()) ? m_peak[i] : -100.0f;

        float rmsFrac = normDB(rmsVal);
        float peakFrac = normDB(peakVal);

        int rmsW = static_cast<int>(rmsFrac * barW);
        int peakX = xStart + static_cast<int>(peakFrac * barW);

        // RMS fill (Gradient)
        QLinearGradient grad(xStart, y, xStart + barW, y);
        grad.setColorAt(0.0, QColor("#34c759"));
        grad.setColorAt(0.55, QColor("#ffcc00"));
        grad.setColorAt(0.75, QColor("#ff9500"));
        grad.setColorAt(0.95, QColor("#ff3b30"));
        p.fillRect(xStart, y, rmsW, barHeight, grad);

        // Peak line indicator
        if (peakFrac > 0) {
            p.setPen(QPen(QColor("#ffffff"), 2));
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

CompactLevelMeterBar::CompactLevelMeterBar(
    std::shared_ptr<MonitoringController> monitoring,
    std::shared_ptr<DSPEngineController> dsp,
    QWidget* parent
) : QWidget(parent), m_monitoring(monitoring), m_dsp(dsp) {
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

    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, [this]() {
        update();
    });
    connect(m_dsp.get(), &DSPEngineController::statusChanged, this, [this](ProcessingState) {
        updateState();
    });
    updateState();
}

void CompactLevelMeterBar::updateState() {
    ProcessingState st = m_dsp->status;
    switch (st) {
    case ProcessingState::Running:
        m_statusDot->setStyleSheet("background-color: #34c759; border-radius: 4px;");
        m_statusLabel->setText("Engine Running");
        m_statusLabel->setStyleSheet("color: #34c759;");
        break;
    case ProcessingState::Paused:
        m_statusDot->setStyleSheet("background-color: #007aff; border-radius: 4px;");
        m_statusLabel->setText("Engine Paused");
        m_statusLabel->setStyleSheet("color: #007aff;");
        break;
    case ProcessingState::Stalled:
        m_statusDot->setStyleSheet("background-color: #ff9500; border-radius: 4px;");
        m_statusLabel->setText("Engine Stalled");
        m_statusLabel->setStyleSheet("color: #ff9500;");
        break;
    case ProcessingState::Starting:
        m_statusDot->setStyleSheet("background-color: #ffcc00; border-radius: 4px;");
        m_statusLabel->setText("Starting Engine...");
        m_statusLabel->setStyleSheet("color: #ffcc00;");
        break;
    case ProcessingState::Inactive:
    default:
        m_statusDot->setStyleSheet("background-color: #8e8e93; border-radius: 4px;");
        m_statusLabel->setText("Engine Inactive");
        m_statusLabel->setStyleSheet("color: #8e8e93;");
        break;
    }
}

void CompactLevelMeterBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    const auto& st = m_monitoring->levelState;

    size_t capCount = st.captureRms.size();
    size_t pbCount = st.playbackRms.size();

    int startX = 16;
    int barW = 120;
    int barH = 6;

    // Draw Capture Meters (Mic icon + bars)
    p.setFont(QFont("sans-serif", 9));
    p.setPen(StyleTheme::textSecondary());
    p.drawText(startX, 22, "🎤 IN:");
    startX += 45;

    for (size_t i = 0; i < std::min(capCount, (size_t)4); ++i) {
        p.fillRect(startX, 15, barW, barH, QColor(255, 255, 255, 20));
        float val = i < st.capturePeak.size() ? st.capturePeak[i] : -100.0f;
        float frac = normDB(val);
        int fillW = static_cast<int>(frac * barW);
        if (fillW > 0) {
            p.fillRect(startX, 15, fillW, barH, QColor("#34c759"));
        }
        startX += barW + 8;
    }

    startX += 16;
    // Draw Playback Meters (Speaker icon + bars)
    p.drawText(startX, 22, "🔊 OUT:");
    startX += 55;

    for (size_t i = 0; i < std::min(pbCount, (size_t)4); ++i) {
        p.fillRect(startX, 15, barW, barH, QColor(255, 255, 255, 20));
        float val = i < st.playbackPeak.size() ? st.playbackPeak[i] : -100.0f;
        float frac = normDB(val);
        int fillW = static_cast<int>(frac * barW);
        if (fillW > 0) {
            p.fillRect(startX, 15, fillW, barH, QColor("#007aff"));
        }
        startX += barW + 8;
    }
}
