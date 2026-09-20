#include "ui/GroupDelayPlotWidget.h"

#include <QBrush>           // for QBrush
#include <QColor>           // for QColor
#include <QFileDialog>      // for QFileDialog
#include <QFont>            // for QFont
#include <QFontDatabase>    // for QFontDatabase
#include <QMessageBox>      // for QMessageBox
#include <QPainter>         // for QPainter
#include <QPainterPath>     // for QPainterPath
#include <QPalette>         // for QPalette
#include <QPen>             // for QPen
#include <QPixmap>          // for QPixmap
#include <QPointF>          // for QPointF
#include <QRect>            // for QRect
#include <QRectF>           // for QRectF
#include <QString>          // for QString, operator+
#include <Qt>               // for AlignmentFlag
#include <QtGlobal>         // for QOverload
#include <algorithm>        // for max, min, sort
#include <cmath>            // for log10, abs
#include <cstdlib>          // for size_t, abs
#include <initializer_list> // for initializer_list
#include <optional>         // for optional

GroupDelayPlotWidget::GroupDelayPlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);

    m_exportBtn = new QPushButton("Export Image…", this);
    m_exportBtn->setFixedSize(110, 26);
    connect(m_exportBtn, &QPushButton::clicked, this, &GroupDelayPlotWidget::onExport);
}

void GroupDelayPlotWidget::setSession(MeasurementSession* session) {
    if (m_session) {
        disconnect(m_session, &MeasurementSession::sessionUpdated, this, nullptr);
    }
    m_session = session;
    if (m_session) {
        connect(m_session, &MeasurementSession::sessionUpdated, this, QOverload<>::of(&QWidget::update));
    }
    update();
}

void GroupDelayPlotWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_exportBtn) {
        m_exportBtn->move(width() - 122, 10);
    }
}

void GroupDelayPlotWidget::onExport() {
    QString fileName = QFileDialog::getSaveFileName(this, "Export Group Delay Plot Image", "group_delay.png",
                                                    "PNG Images (*.png);;JPEG Images (*.jpg)");
    if (!fileName.isEmpty()) {
        QPixmap pixmap = grab();
        if (pixmap.save(fileName)) {
            QMessageBox::information(this, "Export Successful", "Group delay image saved to: " + fileName);
        } else {
            QMessageBox::warning(this, "Export Failed", "Could not save image to specified path.");
        }
    }
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
    if (inBand.empty()) {
        return 1.0;
    }
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
    painter.fillRect(rect(), palette().color(QPalette::Base));

    if (!m_session || !m_session->measuredFR.has_value()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QFont emptyF = font();
        emptyF.setPointSize(12);
        painter.setFont(emptyF);
        painter.drawText(rect(), Qt::AlignCenter, "No frequency response data available for Group Delay plot.");
        return;
    }

    const auto& fr = m_session->measuredFR.value();
    std::vector<double> gd = fr.groupDelay();
    double scaleMs = autoScaleMs(fr, gd);

    // Grid lines - Center Line (0 ms)
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.drawLine(QPointF(0, h / 2.0), QPointF(w, h / 2.0));

    // Axis Labels
    painter.setPen(palette().color(QPalette::PlaceholderText));
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(9);
    painter.setFont(monoFont);
    painter.drawText(QRectF(12, 4, 80, 16), Qt::AlignLeft, QString("+%1 ms").arg(scaleMs, 0, 'f', 1));
    painter.drawText(QRectF(12, h / 2.0 - 18, 60, 16), Qt::AlignLeft, "0 ms");
    painter.drawText(QRectF(12, h - 20, 80, 16), Qt::AlignLeft, QString("-%1 ms").arg(scaleMs, 0, 'f', 1));

    // Frequency Grid Lines
    for (double f : {20.0, 100.0, 1000.0, 10000.0}) {
        double x = freqToX(f, w);
        painter.setPen(QPen(palette().color(QPalette::Mid), 0.5));
        painter.drawLine(QPointF(x, 0), QPointF(x, h));
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QString label =
            (f >= 1000.0) ? QString("%1k").arg(static_cast<int>(f / 1000.0)) : QString::number(static_cast<int>(f));
        double labelX = std::max(4.0, std::min(w - 30.0, x - 12.0));
        painter.drawText(QRectF(labelX, h - 18, 25, 14), Qt::AlignCenter, label);
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

    QColor curveColor = palette().color(QPalette::Highlight);
    painter.setPen(QPen(curveColor, 1.2));
    painter.drawPath(gdPath);

    // Legend
    painter.fillRect(QRect(w - 280, 10, 140, 26), palette().color(QPalette::Window));
    painter.setPen(QPen(curveColor, 1.5));
    painter.drawLine(w - 272, 23, w - 247, 23);
    painter.setPen(palette().color(QPalette::Text));
    QFont legF = font();
    legF.setPointSize(9);
    painter.setFont(legF);
    painter.drawText(w - 239, 27, "Group Delay (ms)");
}
