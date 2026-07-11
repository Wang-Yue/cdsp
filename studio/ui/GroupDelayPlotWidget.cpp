#include "ui/GroupDelayPlotWidget.h"

#include "ui/StyleTheme.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

GroupDelayPlotWidget::GroupDelayPlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);
}

void GroupDelayPlotWidget::setSession(MeasurementSession* session) {
    m_session = session;
    if (m_session) {
        connect(m_session, &MeasurementSession::sessionUpdated, this, QOverload<>::of(&QWidget::update));
    }
    update();
}

double GroupDelayPlotWidget::freqToX(double f, double width) const {
    double minLog = std::log10(20.0);
    double maxLog = std::log10(20000.0);
    double logF = std::log10(std::max(20.0, std::min(20000.0, f)));
    return width * (logF - minLog) / (maxLog - minLog);
}

double GroupDelayPlotWidget::autoScaleMs(const FrequencyResponse& fr, const std::vector<double>& gd) const {
    std::vector<double> inBand;
    size_t bins = fr.bins();
    for (size_t k = 1; k < bins; ++k) {
        double f = fr.frequency(k);
        if (f >= 20.0 && f <= 20000.0 && k < gd.size()) {
            inBand.push_back(std::abs(gd[k]));
        }
    }
    if (inBand.empty())
        return 5.0;

    std::sort(inBand.begin(), inBand.end());
    double p95 = inBand[static_cast<size_t>(static_cast<double>(inBand.size()) * 0.95)];
    return std::max(p95 * 1000.0 * 1.2, 1.0);
}

void GroupDelayPlotWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    double w = width();
    double h = height();

    // Background
    painter.fillRect(rect(), StyleTheme::cardBg());

    if (!m_session || !m_session->measuredFR.has_value()) {
        painter.setPen(StyleTheme::textSecondary());
        painter.setFont(QFont("sans-serif", 12));
        painter.drawText(rect(), Qt::AlignCenter, "No frequency response data available for Group Delay plot.");
        return;
    }

    const auto& fr = m_session->measuredFR.value();
    std::vector<double> gd = fr.groupDelay();
    double scaleMs = autoScaleMs(fr, gd);

    // Center Line (0 ms)
    painter.setPen(QPen(StyleTheme::axisLabelPenColor(), 1.0));
    painter.drawLine(QPointF(0, h / 2.0), QPointF(w, h / 2.0));

    // Axis Labels
    painter.setPen(StyleTheme::textSecondary());
    painter.drawText(QRectF(10, 8, 80, 16), Qt::AlignLeft, QString("+%1 ms").arg(scaleMs, 0, 'f', 1));
    painter.drawText(QRectF(10, h / 2.0 - 18, 60, 16), Qt::AlignLeft, "0 ms");
    painter.drawText(QRectF(10, h - 22, 80, 16), Qt::AlignLeft, QString("-%1 ms").arg(scaleMs, 0, 'f', 1));

    // Frequency Grid
    std::vector<double> gridFreqs = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    for (double f : gridFreqs) {
        double x = freqToX(f, w);
        painter.setPen(QPen(StyleTheme::gridPenColor(), 0.5));
        painter.drawLine(QPointF(x, 0), QPointF(x, h));
        painter.setPen(StyleTheme::textSecondary());
        painter.drawText(QRectF(x + 2, h - 18, 50, 15), Qt::AlignLeft | Qt::AlignBottom,
                         f >= 1000 ? QString::number(f / 1000.0, 'g', 2) + "k" : QString::number(f));
    }

    // Group Delay Curve
    QPainterPath gdPath;
    bool started = false;
    size_t bins = fr.bins();

    for (size_t k = 1; k < bins; ++k) {
        double f = fr.frequency(k);
        if (f < 20.0 || f > 20000.0 || k >= gd.size())
            continue;
        double gdMs = gd[k] * 1000.0;
        double x = freqToX(f, w);
        double y = h * (0.5 - 0.5 * (gdMs / scaleMs));

        if (!started) {
            gdPath.moveTo(x, y);
            started = true;
        } else {
            gdPath.lineTo(x, y);
        }
    }

    painter.setPen(QPen(QColor(0, 180, 255), 1.4));
    painter.drawPath(gdPath);
}
