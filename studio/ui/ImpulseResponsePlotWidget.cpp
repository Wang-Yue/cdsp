#include "ui/ImpulseResponsePlotWidget.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <algorithm>

ImpulseResponsePlotWidget::ImpulseResponsePlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);
}

void ImpulseResponsePlotWidget::setSession(MeasurementSession* session) {
    m_session = session;
    if (m_session) {
        connect(m_session, &MeasurementSession::sessionUpdated, this, QOverload<>::of(&QWidget::update));
    }
    update();
}

void ImpulseResponsePlotWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    double w = width();
    double h = height();

    // Background
    painter.fillRect(rect(), QColor(20, 20, 24));

    if (!m_session || !m_session->measuredIR.has_value()) {
        painter.setPen(QColor(160, 160, 160));
        painter.setFont(QFont("sans-serif", 12));
        painter.drawText(rect(), Qt::AlignCenter, "No impulse response data available.");
        return;
    }

    const auto& ir = m_session->measuredIR.value();
    double halfMs = 50.0;
    int halfSamples = static_cast<int>((halfMs / 1000.0) * static_cast<double>(ir.sampleRate));
    size_t zeroIdx = ir.zeroIndex;
    int lo = std::max(0, static_cast<int>(zeroIdx) - halfSamples);
    int hi = std::min(static_cast<int>(ir.samples.size()) - 1, static_cast<int>(zeroIdx) + halfSamples);

    double peakAbs = 1e-9;
    if (lo <= hi) {
        for (int i = lo; i <= hi; ++i) {
            peakAbs = std::max(peakAbs, std::abs(ir.samples[i]));
        }
    }
    peakAbs *= 1.05;

    // Center horizontal line (y = 0)
    painter.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
    painter.drawLine(QPointF(0, h / 2.0), QPointF(w, h / 2.0));

    // Zero-time vertical line
    double xPeak = w * static_cast<double>(static_cast<int>(zeroIdx) - lo) / static_cast<double>(hi - lo);
    painter.setPen(QPen(QColor(255, 255, 255, 60), 1.0, Qt::DashLine));
    painter.drawLine(QPointF(xPeak, 0), QPointF(xPeak, h));

    // Draw IR curve
    QPainterPath irPath;
    for (int i = lo; i <= hi; ++i) {
        double x = w * static_cast<double>(i - lo) / static_cast<double>(hi - lo);
        double sampleVal = ir.samples[i];
        double y = h * (0.5 - 0.5 * (sampleVal / peakAbs));
        if (i == lo) {
            irPath.moveTo(x, y);
        } else {
            irPath.lineTo(x, y);
        }
    }

    painter.setPen(QPen(QColor(0, 160, 255), 1.2));
    painter.drawPath(irPath);

    // Time-axis Ticks (-50ms, -25ms, 0ms, +25ms, +50ms)
    std::vector<double> ticks = {-halfMs, -halfMs / 2.0, 0.0, halfMs / 2.0, halfMs};
    painter.setPen(QColor(160, 160, 160));
    for (double ms : ticks) {
        int sampleOffset = static_cast<int>((ms / 1000.0) * static_cast<double>(ir.sampleRate));
        int idx = static_cast<int>(zeroIdx) + sampleOffset;
        if (idx >= lo && idx <= hi) {
            double x = w * static_cast<double>(idx - lo) / static_cast<double>(hi - lo);
            painter.drawLine(QPointF(x, h - 6), QPointF(x, h));
            QString label = QString::number(static_cast<int>(ms)) + " ms";
            painter.drawText(QRectF(x - 30, h - 22, 60, 15), Qt::AlignCenter, label);
        }
    }
}
