#include "ui/WaterfallPlotWidget.h"

#include "ui/StyleTheme.h"

#include <QPainterPath>
#include <algorithm>
#include <cmath>

WaterfallPlotWidget::WaterfallPlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);
}

void WaterfallPlotWidget::setSlices(const std::vector<std::pair<double, FrequencyResponse>>& slices) {
    m_slices = slices;
    update();
}

void WaterfallPlotWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());
    if (m_slices.empty())
        return;

    int w = width();
    int h = height();

    double totalDepthY = h * 0.4;
    double totalShiftX = w * 0.15;
    double plotW = w - totalShiftX;
    double plotH = h - totalDepthY;

    double fMin = 20.0, fMax = 1000.0;
    double floorDB = -40.0;
    double logMin = std::log10(fMin);
    double logMax = std::log10(fMax);

    // Reference level (first slice peak)
    double refPeak = 0.0;
    const auto& firstFr = m_slices.front().second;
    for (size_t bin = 0; bin < firstFr.bins(); ++bin) {
        double f = firstFr.frequency(bin);
        if (f >= fMin && f <= fMax) {
            refPeak = std::max(refPeak, firstFr.magnitude(bin));
        }
    }
    double refDB = (refPeak > 0.0) ? 20.0 * std::log10(refPeak) : 0.0;

    // Render back to front for isometric depth
    for (int idx = static_cast<int>(m_slices.size()) - 1; idx >= 0; --idx) {
        const auto& [timeSec, fr] = m_slices[idx];
        double progress =
            static_cast<double>(idx) / static_cast<double>(std::max(1, static_cast<int>(m_slices.size()) - 1));

        double shiftX = totalShiftX * progress;
        double shiftY = totalDepthY * progress;

        QPainterPath path;
        bool isFirst = true;

        size_t count = fr.bins();
        size_t binStride = std::max(static_cast<size_t>(1), count / 800);

        for (size_t bin = 0; bin < count; bin += binStride) {
            double f = fr.frequency(bin);
            if (f < fMin || f > fMax)
                continue;

            double mag = fr.magnitude(bin);
            double db = (mag > 0.0) ? 20.0 * std::log10(mag) - refDB : -100.0;
            double clampedDB = std::max(floorDB, std::min(10.0, db));
            double yFrac = (clampedDB - floorDB) / (10.0 - floorDB);

            double logF = std::log10(f);
            double x = shiftX + (logF - logMin) / (logMax - logMin) * plotW;
            double y = h - shiftY - yFrac * plotH;

            if (isFirst) {
                path.moveTo(x, y);
                isFirst = false;
            } else {
                path.lineTo(x, y);
            }
        }

        if (!path.isEmpty()) {
            // Fill background mask
            QPainterPath fillPath = path;
            fillPath.lineTo(shiftX + plotW, h - shiftY);
            fillPath.lineTo(shiftX, h - shiftY);
            fillPath.closeSubpath();

            QColor maskColor = StyleTheme::cardBg();
            maskColor.setAlpha(240);
            p.fillPath(fillPath, maskColor);

            double hue = 0.6 - 0.5 * (1.0 - progress);
            QColor sliceColor = QColor::fromHsvF(hue, 0.8, 0.9);
            p.setPen(QPen(sliceColor, 1.5));
            p.drawPath(path);
        }
    }
}
