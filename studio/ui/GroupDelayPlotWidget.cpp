#include "ui/GroupDelayPlotWidget.h"

#include "ui/StyleTheme.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

GroupDelayPlotWidget::GroupDelayPlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);

    m_exportBtn = new QPushButton("Export Image…", this);
    m_exportBtn->setFixedSize(110, 26);
    m_exportBtn->setStyleSheet("QPushButton { background: rgba(50, 50, 50, 0.7); color: #e0e0e0; border: 1px solid "
                               "#555; border-radius: 4px; font-size: 11px; }"
                               "QPushButton:hover { background: rgba(80, 80, 80, 0.8); }");
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
    painter.setFont(QFont("Monospace", 9));
    painter.drawText(QRectF(12, 4, 80, 16), Qt::AlignLeft, QString("+%1 ms").arg(scaleMs, 0, 'f', 1));
    painter.drawText(QRectF(12, h / 2.0 - 18, 60, 16), Qt::AlignLeft, "0 ms");
    painter.drawText(QRectF(12, h - 20, 80, 16), Qt::AlignLeft, QString("-%1 ms").arg(scaleMs, 0, 'f', 1));

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

    // Legend
    painter.fillRect(QRect(w - 280, 10, 140, 26),
                     StyleTheme::isDark() ? QColor(0, 0, 0, 150) : QColor(245, 245, 247, 210));
    painter.setPen(QPen(QColor(0, 180, 255), 2));
    painter.drawLine(w - 272, 23, w - 247, 23);
    painter.setPen(StyleTheme::textPrimary());
    painter.setFont(QFont("sans-serif", 9));
    painter.drawText(w - 239, 27, "Group Delay (ms)");
}
