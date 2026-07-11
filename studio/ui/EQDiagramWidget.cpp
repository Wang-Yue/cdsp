#include "ui/EQDiagramWidget.h"
#include "ui/StyleTheme.h"
#include <QPainterPath>
#include <QLinearGradient>
#include <cmath>
#include <algorithm>

EQDiagramWidget::EQDiagramWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
}

void EQDiagramWidget::setSpectrumEngine(std::shared_ptr<SpectrumEngine> spectrum) {
    if (m_spectrum) {
        disconnect(m_spectrum.get(), &SpectrumEngine::updated, this, QOverload<>::of(&QWidget::update));
    }
    m_spectrum = spectrum;
    if (m_spectrum) {
        connect(m_spectrum.get(), &SpectrumEngine::updated, this, QOverload<>::of(&QWidget::update));
    }
    update();
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

    // Card background
    painter.fillRect(rect(), StyleTheme::cardBg());

    // 1. Live Spectrum Analyzer Background Overlay
    if (m_showAnalyzer && m_spectrum && !m_spectrum->data.magnitudes.empty()) {
        const auto& specBands = m_spectrum->data.magnitudes;
        const auto& specFreqs = m_spectrum->data.frequencies;
        if (!specBands.empty() && specBands.size() == specFreqs.size()) {
            QPainterPath fillPath, strokePath;
            fillPath.moveTo(0, h);

            for (size_t i = 0; i < specBands.size(); ++i) {
                double f = specFreqs[i];
                double db = std::max(-24.0, std::min(24.0, static_cast<double>(specBands[i])));
                double x = freqToX(f, w);
                double y = dbToY(db, h);

                if (i == 0) {
                    fillPath.lineTo(x, y);
                    strokePath.moveTo(x, y);
                } else {
                    fillPath.lineTo(x, y);
                    strokePath.lineTo(x, y);
                }
            }
            fillPath.lineTo(w, h);
            fillPath.closeSubpath();

            QLinearGradient grad(0, 0, 0, h);
            grad.setColorAt(0.0, QColor(0, 122, 255, 35));
            grad.setColorAt(1.0, QColor(0, 122, 255, 2));
            painter.fillPath(fillPath, grad);
            painter.setPen(QPen(QColor(0, 122, 255, 90), 1.2));
            painter.drawPath(strokePath);
        }
    }

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

    // Equal-Loudness Contour (ISO 226) Reference Curve Overlay
    if (m_showLoudnessContour) {
        QPainterPath loudnessPath;
        for (int x = 0; x <= w; x += 2) {
            double f = xToFreq(x, w);
            double bassBoost = 6.0 * (1.0 / (1.0 + std::pow(f / 130.0, 2.0)));
            double trebleBoost = 4.0 * (std::pow(f / 5000.0, 2.0) / (1.0 + std::pow(f / 5000.0, 2.0)));
            double db = std::max(-24.0, std::min(24.0, bassBoost + trebleBoost));
            double y = dbToY(db, h);
            if (x == 0) loudnessPath.moveTo(x, y);
            else loudnessPath.lineTo(x, y);
        }
        painter.setPen(QPen(QColor("#ff9500"), 1.5, Qt::DashLine));
        painter.drawPath(loudnessPath);
    }

    // Individual Band Curves
    const QColor colors[] = {
        QColor("#ff3b30"), QColor("#ff9500"), QColor("#ffcc00"),
        QColor("#34c759"), QColor("#007aff"), QColor("#af52de"),
        QColor("#5856d6"), QColor("#ff2d55"), QColor("#a2845e")
    };
    int numColors = 9;

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
        bool isSelected = (static_cast<int>(i) == m_selectedIndex);
        bool isHovered = (static_cast<int>(i) == m_hoveredIndex);
        c.setAlpha(isSelected ? 220 : (isHovered ? 140 : 70));
        painter.setPen(QPen(c, isSelected ? 2.2 : (isHovered ? 1.5 : 1.0)));
        painter.drawPath(path);
    }

    // Combined Response Fill Gradient & Anti-aliased Curve
    QPainterPath totalPath;
    QPainterPath fillPath;
    fillPath.moveTo(0, zeroY);

    for (int x = 0; x <= w; x += 2) {
        double f = xToFreq(x, w);
        double db = m_preset.combinedResponse(f, m_sampleRate);
        double y = dbToY(db, h);
        if (x == 0) {
            totalPath.moveTo(x, y);
        } else {
            totalPath.lineTo(x, y);
        }
        fillPath.lineTo(x, y);
    }
    fillPath.lineTo(w, zeroY);
    fillPath.closeSubpath();

    // Fill Gradient
    QLinearGradient grad(0, 0, 0, h);
    grad.setColorAt(0.0, QColor(0, 122, 255, 45));
    grad.setColorAt(0.5, QColor(0, 122, 255, 15));
    grad.setColorAt(1.0, QColor(0, 122, 255, 3));
    painter.fillPath(fillPath, grad);

    // Combined Curve Line Stroke
    painter.setPen(QPen(QColor("#007aff"), 2.5));
    painter.drawPath(totalPath);

    // Draggable Band Handles & Highlight Rings
    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        const auto& b = m_preset.bands[i];
        if (!b.isEnabled || b.type == EQBandType::Free) continue;

        double handleFreq = b.freq;
        if (b.type == EQBandType::GeneralNotch) handleFreq = b.freqNotch;
        else if (b.type == EQBandType::LinkwitzTransform) handleFreq = b.freqTarget;

        double hx = freqToX(handleFreq, w);
        double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

        bool isSelected = (static_cast<int>(i) == m_selectedIndex);
        bool isHovered = (static_cast<int>(i) == m_hoveredIndex);
        QColor c = colors[i % numColors];

        // Draw selection / hover outer glow ring
        if (isSelected) {
            painter.setBrush(QColor(c.red(), c.green(), c.blue(), 50));
            painter.setPen(QPen(c, 1.5));
            painter.drawEllipse(QPointF(hx, hy), 12, 12);
        } else if (isHovered) {
            painter.setBrush(QColor(c.red(), c.green(), c.blue(), 30));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(hx, hy), 10, 10);
        }

        // Main handle node
        int r = isSelected ? 7 : 5;
        painter.setBrush(c);
        painter.setPen(QPen(Qt::white, isSelected ? 2.5 : 1.2));
        painter.drawEllipse(QPointF(hx, hy), r, r);
    }

    // Parameters Readout Overlay Card
    drawOverlayReadout(painter, w, h);
}

void EQDiagramWidget::drawOverlayReadout(QPainter& painter, int w, int h) {
    Q_UNUSED(h);
    QString text;
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_preset.bands.size())) {
        const auto& b = m_preset.bands[m_selectedIndex];
        double f = b.freq;
        if (b.type == EQBandType::GeneralNotch) f = b.freqNotch;
        else if (b.type == EQBandType::LinkwitzTransform) f = b.freqTarget;

        text = QString("Band #%1 [%2] | Fc: %3 Hz")
                   .arg(m_selectedIndex + 1)
                   .arg(QString::fromStdString(eqBandTypeToString(b.type)))
                   .arg(static_cast<int>(std::round(f)));

        if (eqBandTypeHasGain(b.type)) {
            text += QString(" | Gain: %1%2 dB")
                        .arg(b.gain >= 0 ? "+" : "")
                        .arg(b.gain, 0, 'f', 1);
        }

        if (eqBandTypeHasQ(b.type)) {
            if (b.type == EQBandType::Lowshelf || b.type == EQBandType::Highshelf) {
                if (b.useSlope) text += QString(" | Slope: %1 dB/oct").arg(b.slope, 0, 'f', 1);
                else text += QString(" | Q: %1").arg(b.q, 0, 'f', 2);
            } else if (b.type == EQBandType::Notch || b.type == EQBandType::Bandpass || b.type == EQBandType::Allpass) {
                if (b.useBandwidth) text += QString(" | BW: %1 oct").arg(b.bandwidth, 0, 'f', 2);
                else text += QString(" | Q: %1").arg(b.q, 0, 'f', 2);
            } else {
                text += QString(" | Q: %1").arg(b.q, 0, 'f', 2);
            }
        }
    } else {
        int activeBands = 0;
        for (const auto& b : m_preset.bands) if (b.isEnabled) activeBands++;
        text = QString("Preset: %1 | Preamp: %2%3 dB | Active Bands: %4/%5")
                   .arg(QString::fromStdString(m_preset.name))
                   .arg(m_preset.preampGain >= 0 ? "+" : "")
                   .arg(m_preset.preampGain, 0, 'f', 1)
                   .arg(activeBands)
                   .arg(m_preset.bands.size());
    }

    QFont font("sans-serif", 9, QFont::Medium);
    painter.setFont(font);
    QFontMetrics fm(font);
    int textW = fm.horizontalAdvance(text) + 20;
    int textH = 24;
    int rectX = w - textW - 12;
    int rectY = 10;

    QRect bgRect(rectX, rectY, textW, textH);
    painter.setBrush(QColor(20, 20, 25, 200));
    painter.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    painter.drawRoundedRect(bgRect, 6, 6);

    painter.setPen(QColor(240, 240, 245));
    painter.drawText(bgRect, Qt::AlignCenter, text);
}

void EQDiagramWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int w = width();
        int h = height();

        m_draggingIndex = -1;
        int hitIndex = -1;

        for (size_t i = 0; i < m_preset.bands.size(); ++i) {
            const auto& b = m_preset.bands[i];
            if (!b.isEnabled || b.type == EQBandType::Free) continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch) handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform) handleFreq = b.freqTarget;

            double hx = freqToX(handleFreq, w);
            double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 12.0) {
                hitIndex = static_cast<int>(i);
                break;
            }
        }

        m_draggingIndex = hitIndex;
        m_selectedIndex = hitIndex;
        if (onBandSelected) onBandSelected(hitIndex);
        update();
    }
}

void EQDiagramWidget::mouseMoveEvent(QMouseEvent* event) {
    int w = width();
    int h = height();

    if (m_draggingIndex >= 0 && m_draggingIndex < static_cast<int>(m_preset.bands.size())) {
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
    } else {
        // Hover Detection
        int oldHovered = m_hoveredIndex;
        m_hoveredIndex = -1;

        for (size_t i = 0; i < m_preset.bands.size(); ++i) {
            const auto& b = m_preset.bands[i];
            if (!b.isEnabled || b.type == EQBandType::Free) continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch) handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform) handleFreq = b.freqTarget;

            double hx = freqToX(handleFreq, w);
            double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 12.0) {
                m_hoveredIndex = static_cast<int>(i);
                break;
            }
        }

        if (m_hoveredIndex != oldHovered) {
            setCursor(m_hoveredIndex >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
}

void EQDiagramWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_draggingIndex = -1;
    }
}

void EQDiagramWidget::leaveEvent(QEvent* event) {
    Q_UNUSED(event);
    if (m_hoveredIndex != -1) {
        m_hoveredIndex = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

void EQDiagramWidget::wheelEvent(QWheelEvent* event) {
    int targetIdx = m_selectedIndex;
    if (targetIdx < 0 || targetIdx >= static_cast<int>(m_preset.bands.size())) {
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

