#include "ui/ConvolutionIRPlot.h"

#include "models/ConvCoefficientLoader.h"
#include "ui/StyleTheme.h"

#include <QPainterPath>
#include <algorithm>
#include <cmath>

ConvolutionIRPlot::ConvolutionIRPlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(100);
}

void ConvolutionIRPlot::setIRPath(const std::string& path, const std::string& title) {
    m_path = path;
    m_title = title;
    loadIR();
    update();
}

void ConvolutionIRPlot::setSamples(const std::vector<double>& samples, const std::string& title) {
    m_samples = samples;
    m_title = title;
    m_errorMsg.clear();
    update();
}

void ConvolutionIRPlot::loadIR() {
    if (m_path.empty()) {
        m_samples.clear();
        m_errorMsg = "No file path provided.";
        return;
    }
    m_samples = ConvCoefficientLoader::loadCoefficients(m_path, "AUTO", 0, 48000);
    if (m_samples.empty()) {
        m_errorMsg = "Could not load IR file.";
    } else {
        m_errorMsg.clear();
    }
}

void ConvolutionIRPlot::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    // Clip rounded rect background
    QPainterPath bgPath;
    bgPath.addRoundedRect(rect(), 6, 6);
    p.setClipPath(bgPath);
    p.fillRect(rect(), StyleTheme::cardBg());

    int plotTop = 4;
    if (!m_title.empty()) {
        p.setFont(QFont("sans-serif", 10, QFont::Bold));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(8, 18, QString::fromStdString(m_title));
        plotTop = 24;
    }

    if (!m_errorMsg.empty()) {
        p.setFont(QFont("sans-serif", 11));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(8, plotTop + 16, QString::fromStdString(m_errorMsg));
        return;
    }

    if (m_samples.empty())
        return;

    int plotH = h - plotTop - 4;
    int midY = plotTop + plotH / 2;

    // Zero baseline (0.18 opacity = 46 alpha)
    QColor baselineCol = StyleTheme::isDark() ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 46);
    p.setPen(QPen(baselineCol, 1, Qt::SolidLine));
    p.drawLine(0, midY, w, midY);

    double maxVal = 1e-9;
    for (double v : m_samples) {
        maxVal = std::max(maxVal, std::abs(v));
    }

    size_t count = m_samples.size();
    QPainterPath waveformPath;

    for (size_t i = 0; i < count; ++i) {
        double norm = m_samples[i] / maxVal;
        double x = (static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, count - 1))) * w;
        double y = midY - norm * (plotH / 2.0);

        if (i == 0)
            waveformPath.moveTo(x, y);
        else
            waveformPath.lineTo(x, y);
    }

    p.setPen(QPen(StyleTheme::accent(), 1.0));
    p.drawPath(waveformPath);
}
