#include "ui/ConvolutionIRPlot.h"

#include "models/ConvCoefficientLoader.h"
#include "ui/StyleTheme.h"

#include <QPainterPath>
#include <algorithm>
#include <cmath>

ConvolutionIRPlot::ConvolutionIRPlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(120);
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

    p.fillRect(rect(), StyleTheme::cardBg());

    if (!m_title.empty()) {
        p.setFont(QFont("sans-serif", 9, QFont::Bold));
        p.setPen(StyleTheme::textSecondary());
        p.drawText(8, 16, QString::fromStdString(m_title));
    }

    if (!m_errorMsg.empty()) {
        p.setFont(QFont("sans-serif", 9));
        p.setPen(QColor("#ff453a"));
        p.drawText(16, h / 2, QString::fromStdString(m_errorMsg));
        return;
    }

    if (m_samples.empty())
        return;

    int plotTop = m_title.empty() ? 4 : 22;
    int plotH = h - plotTop - 4;

    // Zero axis line (0.18 opacity = 46 alpha)
    int midY = plotTop + plotH / 2;
    QColor baselineCol = StyleTheme::isDark() ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 46);
    p.setPen(QPen(baselineCol, 1, Qt::SolidLine));
    p.drawLine(0, midY, w, midY);

    double maxVal = 1e-9;
    for (double v : m_samples) {
        maxVal = std::max(maxVal, std::abs(v));
    }

    size_t count = m_samples.size();
    QPainterPath path;

    for (size_t i = 0; i < count; ++i) {
        double norm = m_samples[i] / maxVal;
        double x = (static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, count - 1))) * w;
        double y = midY - norm * (plotH / 2.0);

        if (i == 0)
            path.moveTo(x, y);
        else
            path.lineTo(x, y);
    }

    p.setPen(QPen(QColor("#007aff"), 1.0));
    p.drawPath(path);
}
