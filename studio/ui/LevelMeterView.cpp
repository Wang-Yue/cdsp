#include "ui/LevelMeterView.h"
#include "ui/StyleTheme.h"
#include <cmath>
#include <algorithm>

LevelMeterView::LevelMeterView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(140);
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
    int barHeight = (barAreaHeight - static_cast<int>(chCount - 1) * 8) / static_cast<int>(chCount);
    barHeight = std::max(12, std::min(24, barHeight));

    for (size_t i = 0; i < chCount; ++i) {
        int y = barAreaTop + static_cast<int>(i) * (barHeight + 8);
        int xStart = 80;
        int barW = w - xStart - 20;

        p.setFont(QFont("sans-serif", 10, QFont::Bold));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(16, y + barHeight - 4, QString("CH %1").arg(i + 1));

        // Background track
        p.fillRect(xStart, y, barW, barHeight, QColor("#16161a"));

        float rmsVal = (i < m_rms.size()) ? m_rms[i] : -100.0f;
        float peakVal = (i < m_peak.size()) ? m_peak[i] : -100.0f;

        float rmsFrac = std::max(0.0f, std::min(1.0f, (rmsVal + 60.0f) / 60.0f));
        float peakFrac = std::max(0.0f, std::min(1.0f, (peakVal + 60.0f) / 60.0f));

        int rmsW = static_cast<int>(rmsFrac * barW);
        int peakX = xStart + static_cast<int>(peakFrac * barW);

        // RMS fill
        QLinearGradient grad(xStart, y, xStart + barW, y);
        grad.setColorAt(0.0, QColor("#2cb67d"));
        grad.setColorAt(0.7, QColor("#ffc72c"));
        grad.setColorAt(1.0, QColor("#ff453a"));
        p.fillRect(xStart, y, rmsW, barHeight, grad);

        // Peak line
        p.setPen(QPen(QColor("#ffffff"), 2));
        p.drawLine(peakX, y, peakX, y + barHeight);
    }
}
