#include "ui/EQDiagramWidget.h"
#include "ui/StyleTheme.h"
#include <QPainterPath>
#include <cmath>
#include <algorithm>

EQDiagramWidget::EQDiagramWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
}

void EQDiagramWidget::setPreset(const EQPreset& preset, int sampleRate) {
    m_preset = preset;
    m_sampleRate = sampleRate;
    update();
}

void EQDiagramWidget::setSelectedBandIndex(int index) {
    m_selectedIndex = index;
    update();
}

double EQDiagramWidget::freqToX(double f, double width) const {
    double minLog = std::log10(fMin);
    double maxLog = std::log10(fMax);
    double logF = std::log10(std::max(fMin, std::min(fMax, f)));
    return width * (logF - minLog) / (maxLog - minLog);
}

double EQDiagramWidget::xToFreq(double x, double width) const {
    double minLog = std::log10(fMin);
    double maxLog = std::log10(fMax);
    double ratio = std::max(0.0, std::min(1.0, x / width));
    return std::pow(10.0, minLog + ratio * (maxLog - minLog));
}

double EQDiagramWidget::dbToY(double db, double height) const {
    double ratio = (db - dbMin) / (dbMax - dbMin);
    return height * (1.0 - ratio);
}

double EQDiagramWidget::yToDb(double y, double height) const {
    double ratio = 1.0 - (y / height);
    return dbMin + ratio * (dbMax - dbMin);
}

void EQDiagramWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    // Background
    painter.fillRect(rect(), StyleTheme::cardBg());

    // Grid Lines
    painter.setPen(QPen(QColor("#d1d1d6"), 0.5, Qt::DashLine));
    for (double db = -18.0; db <= 18.0; db += 6.0) {
        double y = dbToY(db, h);
        painter.drawLine(0, y, w, y);
        painter.drawText(8, y - 4, QString("%1 dB").arg(static_cast<int>(db)));
    }
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        double x = freqToX(f, w);
        painter.drawLine(x, 0, x, h);
        QString label = (f >= 1000.0) ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        painter.drawText(x - 10, h - 8, label);
    }

    // 0 dB Baseline
    painter.setPen(QPen(QColor("#8e8e93"), 1.0));
    double zeroY = dbToY(0.0, h);
    painter.drawLine(0, zeroY, w, zeroY);

    // Individual Band Curves
    const QColor colors[] = { QColor("#ff3b30"), QColor("#ff9500"), QColor("#ffcc00"), QColor("#34c759"), QColor("#007aff"), QColor("#af52de") };
    int numColors = 6;

    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        const auto& band = m_preset.bands[i];
        if (!band.isEnabled) continue;

        QPainterPath path;
        for (int x = 0; x <= w; x += 2) {
            double f = xToFreq(x, w);
            double db = band.response(f, m_sampleRate);
            double y = dbToY(db, h);
            if (x == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        QColor c = colors[i % numColors];
        c.setAlpha(static_cast<int>(i) == m_selectedIndex ? 200 : 70);
        painter.setPen(QPen(c, static_cast<int>(i) == m_selectedIndex ? 2.0 : 1.0));
        painter.drawPath(path);
    }

    // Combined Response Curve
    QPainterPath totalPath;
    for (int x = 0; x <= w; x += 2) {
        double f = xToFreq(x, w);
        double db = m_preset.combinedResponse(f, m_sampleRate);
        double y = dbToY(db, h);
        if (x == 0) totalPath.moveTo(x, y);
        else totalPath.lineTo(x, y);
    }
    painter.setPen(QPen(QColor("#007aff"), 2.5));
    painter.drawPath(totalPath);

    // Draggable Band Handles
    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        const auto& b = m_preset.bands[i];
        if (!b.isEnabled || b.type == EQBandType::Free) continue;

        double handleFreq = b.freq;
        if (b.type == EQBandType::GeneralNotch) handleFreq = b.freqNotch;
        else if (b.type == EQBandType::LinkwitzTransform) handleFreq = b.freqTarget;

        double hx = freqToX(handleFreq, w);
        double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

        QColor c = colors[i % numColors];
        int r = (static_cast<int>(i) == m_selectedIndex) ? 7 : 5;

        painter.setBrush(c);
        painter.setPen(QPen(Qt::white, (static_cast<int>(i) == m_selectedIndex) ? 2.0 : 1.0));
        painter.drawEllipse(QPointF(hx, hy), r, r);
    }
}

void EQDiagramWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int w = width();
        int h = height();

        m_draggingIndex = -1;
        for (size_t i = 0; i < m_preset.bands.size(); ++i) {
            const auto& b = m_preset.bands[i];
            if (!b.isEnabled || b.type == EQBandType::Free) continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch) handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform) handleFreq = b.freqTarget;

            double hx = freqToX(handleFreq, w);
            double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 12.0) {
                m_draggingIndex = static_cast<int>(i);
                m_selectedIndex = static_cast<int>(i);
                if (onBandSelected) onBandSelected(m_selectedIndex);
                update();
                break;
            }
        }
    }
}

void EQDiagramWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_draggingIndex >= 0 && m_draggingIndex < static_cast<int>(m_preset.bands.size())) {
        int w = width();
        int h = height();

        double f = xToFreq(event->position().x(), w);
        double db = yToDb(event->position().y(), h);

        f = std::max(20.0, std::min(20000.0, f));
        db = std::max(-20.0, std::min(20.0, db));

        auto& b = m_preset.bands[m_draggingIndex];
        if (b.type == EQBandType::GeneralNotch) b.freqNotch = f;
        else if (b.type == EQBandType::LinkwitzTransform) b.freqTarget = f;
        else b.freq = f;

        if (eqBandTypeHasGain(b.type)) b.gain = std::round(db * 2.0) / 2.0;

        if (onBandDragged) {
            onBandDragged(m_draggingIndex, f, b.gain);
        }
        update();
    }
}

void EQDiagramWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_draggingIndex = -1;
    }
}

void EQDiagramWidget::wheelEvent(QWheelEvent* event) {
    int targetIdx = m_selectedIndex;
    if (targetIdx < 0 || targetIdx >= static_cast<int>(m_preset.bands.size())) {
        // Find handle under cursor if no node selected
        int w = width();
        int h = height();
        for (size_t i = 0; i < m_preset.bands.size(); ++i) {
            const auto& b = m_preset.bands[i];
            if (!b.isEnabled || b.type == EQBandType::Free) continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch) handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform) handleFreq = b.freqTarget;

            double hx = freqToX(handleFreq, w);
            double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 16.0) {
                targetIdx = static_cast<int>(i);
                break;
            }
        }
    }

    if (targetIdx >= 0 && targetIdx < static_cast<int>(m_preset.bands.size())) {
        auto& band = m_preset.bands[targetIdx];
        if (eqBandTypeHasQ(band.type)) {
            double delta = event->angleDelta().y();
            if (std::abs(delta) > 0.01) {
                double factor = delta > 0 ? 1.05 : 0.95;
                band.q = std::max(0.1, std::min(20.0, band.q * factor));
                if (onBandQChanged) {
                    onBandQChanged(targetIdx, band.q);
                }
                update();
            }
        }
    }
}
