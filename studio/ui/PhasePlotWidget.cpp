#include "ui/PhasePlotWidget.h"

#include "ui/StyleTheme.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

PhasePlotWidget::PhasePlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);

    m_unwrapBtn = new QPushButton("Unwrap Phase", this);
    m_unwrapBtn->setCheckable(true);
    m_unwrapBtn->setFixedSize(110, 26);
    m_unwrapBtn->setStyleSheet("QPushButton { background: rgba(50, 50, 50, 0.7); color: #e0e0e0; border: 1px solid "
                               "#555; border-radius: 4px; font-size: 11px; }"
                               "QPushButton:checked { background: #007acc; color: white; border-color: #0099ff; }");

    connect(m_unwrapBtn, &QPushButton::toggled, [this](bool checked) {
        m_unwrapPhase = checked;
        m_unwrapBtn->setText(checked ? "Wrap Phase" : "Unwrap Phase");
        update();
    });

    m_exportBtn = new QPushButton("Export Image…", this);
    m_exportBtn->setFixedSize(110, 26);
    m_exportBtn->setStyleSheet("QPushButton { background: rgba(50, 50, 50, 0.7); color: #e0e0e0; border: 1px solid "
                               "#555; border-radius: 4px; font-size: 11px; }"
                               "QPushButton:hover { background: rgba(80, 80, 80, 0.8); }");
    connect(m_exportBtn, &QPushButton::clicked, this, &PhasePlotWidget::onExport);
}

void PhasePlotWidget::setSession(MeasurementSession* session) {
    if (m_session) {
        disconnect(m_session, &MeasurementSession::sessionUpdated, this, nullptr);
    }
    m_session = session;
    if (m_session) {
        connect(m_session, &MeasurementSession::sessionUpdated, this, QOverload<>::of(&QWidget::update));
    }
    update();
}

double PhasePlotWidget::freqToX(double f, double width) const {
    double minLog = std::log10(20.0);
    double maxLog = std::log10(20000.0);
    double logF = std::log10(std::max(20.0, std::min(20000.0, f)));
    return width * (logF - minLog) / (maxLog - minLog);
}

double PhasePlotWidget::wrapToPi(double radians) const {
    double r = radians;
    while (r > M_PI)
        r -= 2.0 * M_PI;
    while (r <= -M_PI)
        r += 2.0 * M_PI;
    return r;
}

void PhasePlotWidget::phaseBounds(const FrequencyResponse& fr, const std::vector<double>& unwrapped, double& minDeg,
                                  double& maxDeg) const {
    if (!m_unwrapPhase || unwrapped.empty()) {
        minDeg = -180.0;
        maxDeg = 180.0;
        return;
    }

    std::vector<double> allDegs;
    size_t bins = fr.bins();
    for (size_t k = 1; k < bins; ++k) {
        double f = fr.frequency(k);
        if (f < 20.0 || f > 20000.0)
            continue;
        double deg = unwrapped[k] * 180.0 / M_PI;
        allDegs.push_back(deg);

        if (m_session && m_session->correctionPreset.has_value()) {
            const auto& preset = m_session->correctionPreset.value();
            if (!preset.bands.empty()) {
                double eqPhase = preset.combinedPhase(f, m_session->sampleRate);
                double cDeg = (unwrapped[k] + eqPhase) * 180.0 / M_PI;
                allDegs.push_back(cDeg);
            }
        }
    }

    if (allDegs.empty()) {
        minDeg = -180.0;
        maxDeg = 180.0;
        return;
    }

    auto minIt = std::min_element(allDegs.begin(), allDegs.end());
    auto maxIt = std::max_element(allDegs.begin(), allDegs.end());
    double span = std::max(360.0, *maxIt - *minIt);
    double center = (*maxIt + *minIt) / 2.0;
    minDeg = center - span / 2.0 - 45.0;
    maxDeg = center + span / 2.0 + 45.0;
}

void PhasePlotWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_unwrapBtn) {
        m_unwrapBtn->move(12, height() - 34);
    }
    if (m_exportBtn) {
        m_exportBtn->move(width() - 122, 10);
    }
}

void PhasePlotWidget::onExport() {
    QString fileName = QFileDialog::getSaveFileName(this, "Export Phase Plot Image", "phase_plot.png",
                                                    "PNG Images (*.png);;JPEG Images (*.jpg)");
    if (!fileName.isEmpty()) {
        QPixmap pixmap = grab();
        if (pixmap.save(fileName)) {
            QMessageBox::information(this, "Export Successful", "Phase plot image saved to: " + fileName);
        } else {
            QMessageBox::warning(this, "Export Failed", "Could not save image to specified path.");
        }
    }
}

void PhasePlotWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    // Background
    painter.fillRect(rect(), StyleTheme::cardBg());

    if (!m_session || !m_session->measuredFR.has_value()) {
        painter.setPen(StyleTheme::textSecondary());
        painter.setFont(QFont("sans-serif", 12));
        painter.drawText(rect(), Qt::AlignCenter, "No frequency response available for Phase plot.");
        return;
    }

    const auto& fr = m_session->measuredFR.value();
    std::vector<double> unwrapped = m_unwrapPhase ? fr.unwrappedPhase() : std::vector<double>();

    double minDeg = -180.0, maxDeg = 180.0;
    phaseBounds(fr, unwrapped, minDeg, maxDeg);
    double spanDeg = maxDeg - minDeg;

    // Draw Grid Lines (10 log frequency ticks matching SwiftUI & EQDiagramWidget)
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        double x = freqToX(f, w);
        painter.setPen(QPen(StyleTheme::gridPenColor(), 0.5));
        painter.drawLine(QPointF(x, 0), QPointF(x, h));
        painter.setPen(StyleTheme::textSecondary());
        QString label = (f >= 1000.0) ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        painter.drawText(QRectF(x - 12, h - 18, 25, 14), Qt::AlignCenter, label);
    }

    double degStep = spanDeg > 2880 ? 1440 : (spanDeg > 1440 ? 720 : (spanDeg > 720 ? 360 : 90));
    int startDeg = static_cast<int>(minDeg / degStep) * degStep;
    int endDeg = static_cast<int>(maxDeg / degStep) * degStep;

    for (int deg = startDeg; deg <= endDeg; deg += static_cast<int>(degStep)) {
        double y = h * (1.0 - (static_cast<double>(deg) - minDeg) / spanDeg);
        if (y >= 0 && y <= h) {
            painter.setPen(deg == 0 ? QPen(StyleTheme::axisLabelPenColor(), 1.0)
                                    : QPen(StyleTheme::gridPenColor(), 0.5));
            painter.drawLine(QPointF(0, y), QPointF(w, y));
            painter.setPen(StyleTheme::textSecondary());
            painter.drawText(QRectF(8, y - 12, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, QString::number(deg) + "°");
        }
    }

    // Measured Phase Path (Blue)
    QPainterPath measuredPath;
    bool started = false;
    size_t bins = fr.bins();

    for (size_t k = 1; k < bins; ++k) {
        double f = fr.frequency(k);
        if (f < 20.0 || f > 20000.0)
            continue;
        double phaseRads = unwrapped.empty() ? fr.phase(k) : unwrapped[k];
        double phaseDeg = phaseRads * 180.0 / M_PI;
        double x = freqToX(f, w);
        double y = h * (1.0 - (phaseDeg - minDeg) / spanDeg);

        if (!started) {
            measuredPath.moveTo(x, y);
            started = true;
        } else {
            measuredPath.lineTo(x, y);
        }
    }

    painter.setPen(QPen(QColor(0, 150, 255), 1.5));
    painter.drawPath(measuredPath);

    // Corrected Phase Path (Orange) if preset exists
    if (m_session->correctionPreset.has_value() && !m_session->correctionPreset->bands.empty()) {
        const auto& preset = m_session->correctionPreset.value();
        QPainterPath correctedPath;
        started = false;

        for (size_t k = 1; k < bins; ++k) {
            double f = fr.frequency(k);
            if (f < 20.0 || f > 20000.0)
                continue;
            double baseRads = unwrapped.empty() ? fr.phase(k) : unwrapped[k];
            double eqRads = preset.combinedPhase(f, m_session->sampleRate);
            double totalRads = baseRads + eqRads;
            double activeRads = unwrapped.empty() ? wrapToPi(totalRads) : totalRads;
            double phaseDeg = activeRads * 180.0 / M_PI;
            double x = freqToX(f, w);
            double y = h * (1.0 - (phaseDeg - minDeg) / spanDeg);

            if (!started) {
                correctedPath.moveTo(x, y);
                started = true;
            } else {
                correctedPath.lineTo(x, y);
            }
        }

        painter.setPen(QPen(QColor(255, 140, 0), 1.6));
        painter.drawPath(correctedPath);
    }

    // Legend
    int legendHeight =
        (m_session->correctionPreset.has_value() && !m_session->correctionPreset->bands.empty()) ? 42 : 24;
    painter.fillRect(QRect(w - 315, 8, 180, legendHeight),
                     StyleTheme::isDark() ? QColor(0, 0, 0, 150) : QColor(245, 245, 247, 210));
    painter.setFont(QFont("Monospace", 9));
    painter.setPen(QPen(QColor(0, 150, 255), 1.5));
    painter.drawLine(w - 307, 18, w - 289, 18);
    painter.setPen(StyleTheme::textSecondary());
    painter.drawText(w - 283, 21, "Measured");

    if (m_session->correctionPreset.has_value() && !m_session->correctionPreset->bands.empty()) {
        painter.setPen(QPen(QColor(255, 140, 0), 1.6));
        painter.drawLine(w - 307, 34, w - 289, 34);
        painter.setPen(StyleTheme::textSecondary());
        painter.drawText(w - 283, 37, "Corrected (measured + EQ)");
    }
}
