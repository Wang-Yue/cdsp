#include "ui/ImpulseResponsePlotWidget.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

ImpulseResponsePlotWidget::ImpulseResponsePlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);

    m_exportBtn = new QPushButton("Export Image…", this);
    m_exportBtn->setFixedSize(110, 26);
    connect(m_exportBtn, &QPushButton::clicked, this, &ImpulseResponsePlotWidget::onExport);
}

void ImpulseResponsePlotWidget::setSession(MeasurementSession* session) {
    if (m_session) {
        disconnect(m_session, &MeasurementSession::sessionUpdated, this, nullptr);
    }
    m_session = session;
    if (m_session) {
        connect(m_session, &MeasurementSession::sessionUpdated, this, QOverload<>::of(&QWidget::update));
    }
    update();
}

void ImpulseResponsePlotWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_exportBtn) {
        m_exportBtn->move(width() - 122, 10);
    }
}

void ImpulseResponsePlotWidget::onExport() {
    QString fileName = QFileDialog::getSaveFileName(this, "Export Impulse Response Plot Image", "impulse_response.png",
                                                    "PNG Images (*.png);;JPEG Images (*.jpg)");
    if (!fileName.isEmpty()) {
        QPixmap pixmap = grab();
        if (pixmap.save(fileName)) {
            QMessageBox::information(this, "Export Successful", "Impulse response image saved to: " + fileName);
        } else {
            QMessageBox::warning(this, "Export Failed", "Could not save image to specified path.");
        }
    }
}

void ImpulseResponsePlotWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    double w = width();
    double h = height();

    // Background
    painter.fillRect(rect(), palette().color(QPalette::Base));

    if (!m_session || !m_session->measuredIR.has_value()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
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

    // Zero line (y = 0 horizontal line at h / 2)
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.drawLine(QPointF(0, h / 2.0), QPointF(w, h / 2.0));

    // Zero-time vertical line (peak line t=0)
    double xPeak = w * static_cast<double>(static_cast<int>(zeroIdx) - lo) / static_cast<double>(hi - lo);
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.drawLine(QPointF(xPeak, 0), QPointF(xPeak, h));

    // Normalized Amplitude Y axis [-1.0, +1.0] Labels
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.setFont(QFont("Monospace", 9));
    painter.drawText(QRectF(12, 4, 60, 16), Qt::AlignLeft, "+1.0");
    painter.drawText(QRectF(12, h / 2.0 - 18, 60, 16), Qt::AlignLeft, "0.0");
    painter.drawText(QRectF(12, h - 20, 60, 16), Qt::AlignLeft, "-1.0");

    // Draw IR curve
    QPainterPath irPath;
    double yPeak = h * 0.5;
    for (int i = lo; i <= hi; ++i) {
        double x = w * static_cast<double>(i - lo) / static_cast<double>(hi - lo);
        double sampleVal = ir.samples[i];
        double y = h * (0.5 - 0.5 * (sampleVal / peakAbs));
        if (i == static_cast<int>(zeroIdx)) {
            yPeak = y;
        }
        if (i == lo) {
            irPath.moveTo(x, y);
        } else {
            irPath.lineTo(x, y);
        }
    }

    QColor curveColor = palette().color(QPalette::Highlight);
    painter.setPen(QPen(curveColor, 1.2));
    painter.drawPath(irPath);

    // Peak Marker Dot at (xPeak, yPeak)
    painter.setBrush(curveColor);
    painter.setPen(QPen(Qt::white, 1.0));
    painter.drawEllipse(QPointF(xPeak, yPeak), 3.5, 3.5);

    // Time-axis Ticks (-50ms, -25ms, 0ms, +25ms, +50ms)
    std::vector<double> ticks = {-halfMs, -halfMs / 2.0, 0.0, halfMs / 2.0, halfMs};
    painter.setPen(palette().color(QPalette::PlaceholderText));
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

    // Legend
    painter.fillRect(QRect(w - 280, 10, 150, 26), palette().color(QPalette::Window));
    painter.setPen(QPen(curveColor, 2));
    painter.drawLine(w - 272, 23, w - 247, 23);
    painter.setPen(palette().color(QPalette::Text));
    painter.setFont(QFont("sans-serif", 9));
    painter.drawText(w - 239, 27, "Impulse Response");
}
