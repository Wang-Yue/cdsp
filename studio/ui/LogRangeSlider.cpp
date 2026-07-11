#include "ui/LogRangeSlider.h"

#include "ui/StyleTheme.h"

#include <algorithm>
#include <cmath>

LogRangeSlider::LogRangeSlider(QWidget* parent) : QWidget(parent) {
    setFixedHeight(28);
    setMinimumWidth(200);
}

void LogRangeSlider::setRange(double minFreq, double maxFreq) {
    m_minFreq = std::max(m_minBound, std::min(m_maxBound, minFreq));
    m_maxFreq = std::max(m_minFreq + 10.0, std::min(m_maxBound, maxFreq));
    update();
}

void LogRangeSlider::setMinMaxBounds(double minBound, double maxBound) {
    m_minBound = std::max(1.0, minBound);
    m_maxBound = std::max(m_minBound + 1.0, maxBound);
    setRange(m_minFreq, m_maxFreq);
}

double LogRangeSlider::posToFreq(int x) const {
    int margin = 10;
    int w = width() - 2 * margin;
    if (w <= 0)
        return m_minBound;

    double frac = std::max(0.0, std::min(1.0, static_cast<double>(x - margin) / w));
    double logMin = std::log10(m_minBound);
    double logMax = std::log10(m_maxBound);
    double logFreq = logMin + frac * (logMax - logMin);
    return std::round(std::pow(10.0, logFreq));
}

int LogRangeSlider::freqToPos(double freq) const {
    int margin = 10;
    int w = width() - 2 * margin;
    double logMin = std::log10(m_minBound);
    double logMax = std::log10(m_maxBound);
    double logFreq = std::log10(std::max(m_minBound, std::min(m_maxBound, freq)));
    double frac = (logFreq - logMin) / (logMax - logMin);
    return margin + static_cast<int>(frac * w);
}

void LogRangeSlider::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int margin = 10;
    int yMid = height() / 2;
    int w = width() - 2 * margin;

    // Track background
    p.setPen(QPen(QColor(255, 255, 255, 20), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(margin, yMid, margin + w, yMid);

    // Selected active range track
    int xMin = freqToPos(m_minFreq);
    int xMax = freqToPos(m_maxFreq);

    p.setPen(QPen(QColor("#007aff"), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(xMin, yMid, xMax, yMid);

    // Min and Max Handles
    p.setPen(QPen(QColor("#ffffff"), 2));
    p.setBrush(QColor("#007aff"));
    p.drawEllipse(QPoint(xMin, yMid), 7, 7);
    p.drawEllipse(QPoint(xMax, yMid), 7, 7);
}

void LogRangeSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int x = event->pos().x();
        int xMin = freqToPos(m_minFreq);
        int xMax = freqToPos(m_maxFreq);

        if (std::abs(x - xMin) <= 12) {
            m_activeHandle = 1;
        } else if (std::abs(x - xMax) <= 12) {
            m_activeHandle = 2;
        } else {
            // Pick closer handle
            if (std::abs(x - xMin) < std::abs(x - xMax)) {
                m_activeHandle = 1;
                m_minFreq = std::min(posToFreq(x), m_maxFreq - 10.0);
            } else {
                m_activeHandle = 2;
                m_maxFreq = std::max(posToFreq(x), m_minFreq + 10.0);
            }
            emit rangeChanged(m_minFreq, m_maxFreq);
            update();
        }
        event->accept();
    }
}

void LogRangeSlider::mouseMoveEvent(QMouseEvent* event) {
    if (m_activeHandle > 0 && (event->buttons() & Qt::LeftButton)) {
        double freq = posToFreq(event->pos().x());
        if (m_activeHandle == 1) {
            m_minFreq = std::min(freq, m_maxFreq - 10.0);
        } else if (m_activeHandle == 2) {
            m_maxFreq = std::max(freq, m_minFreq + 10.0);
        }
        emit rangeChanged(m_minFreq, m_maxFreq);
        update();
        event->accept();
    }
}

void LogRangeSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_activeHandle = 0;
        event->accept();
    }
}
