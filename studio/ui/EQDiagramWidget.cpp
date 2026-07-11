#include "ui/EQDiagramWidget.h"

#include "ui/StyleTheme.h"

#include <QLinearGradient>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

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

QColor EQDiagramWidget::bandColor(int index) {
    static const QColor colors[] = {QColor("#ff3b30"), QColor("#ff9500"), QColor("#ffcc00"),
                                    QColor("#34c759"), QColor("#007aff"), QColor("#af52de"),
                                    QColor("#5856d6"), QColor("#ff2d55"), QColor("#a2845e")};
    return colors[std::abs(index) % 9];
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
            double peakDB = -120.0;
            double sumDB = 0.0;
            for (float val : specBands) {
                double v = static_cast<double>(val);
                peakDB = std::max(peakDB, v);
                sumDB += v;
            }
            double offset = (peakDB < -95.0) ? 0.0 : (sumDB / specBands.size());

            QPainterPath fillPath, strokePath;
            fillPath.moveTo(0, h);

            for (size_t i = 0; i < specBands.size(); ++i) {
                double f = specFreqs[i];
                double rawDb = static_cast<double>(specBands[i]);
                double dbVal = (peakDB < -95.0) ? rawDb : (rawDb - offset);
                double dbClamped = std::max(-24.0, std::min(24.0, dbVal));
                double x = freqToX(f, w);
                double y = dbToY(dbClamped, h);

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
    for (double db = -18.0; db <= 18.0; db += 6.0) {
        double y = dbToY(db, h);
        painter.setPen(QPen(StyleTheme::gridPenColor(), 0.5, Qt::DashLine));
        painter.drawLine(0, y, w, y);
        painter.setPen(StyleTheme::textSecondary());
        painter.drawText(8, y - 4, QString("%1 dB").arg(static_cast<int>(db)));
    }
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        double x = freqToX(f, w);
        painter.setPen(QPen(StyleTheme::gridPenColor(), 0.5, Qt::DashLine));
        painter.drawLine(x, 0, x, h);
        painter.setPen(StyleTheme::textSecondary());
        QString label = (f >= 1000.0) ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        painter.drawText(x - 10, h - 8, label);
    }

    // 0 dB Baseline
    painter.setPen(QPen(StyleTheme::axisLabelPenColor(), 1.0));
    double zeroY = dbToY(0.0, h);
    painter.drawLine(0, zeroY, w, zeroY);

    // Equal-Loudness Contour (ISO 226) Reference Curve Overlay
    if (m_showLoudnessContour) {
        double lowBoost = 6.0;
        double highBoost = 4.0;

        if (m_pipelineStore) {
            for (const auto& stage : m_pipelineStore->stages) {
                if (stage.type == StageType::Loudness && stage.isEnabled) {
                    lowBoost = stage.loudnessLowBoost;
                    highBoost = stage.loudnessHighBoost;
                    break;
                }
            }
        }

        QPainterPath loudnessPath;
        for (int x = 0; x <= w; x += 2) {
            double f = xToFreq(x, w);
            double bassBoost = lowBoost * (1.0 / (1.0 + std::pow(f / 130.0, 2.0)));
            double trebleBoost = highBoost * (std::pow(f / 5000.0, 2.0) / (1.0 + std::pow(f / 5000.0, 2.0)));
            double db = std::max(-24.0, std::min(24.0, bassBoost + trebleBoost));
            double y = dbToY(db, h);
            if (x == 0)
                loudnessPath.moveTo(x, y);
            else
                loudnessPath.lineTo(x, y);
        }
        painter.setPen(QPen(QColor("#ff9500"), 1.5, Qt::DashLine));
        painter.drawPath(loudnessPath);
    }

    // Reference Target & Measured Frequency Response Curves Overlay
    if (m_overlay.active) {
        // 1. Draw Target Curve (Dashed Amber)
        if (!m_overlay.targetCurve.breakpoints.empty()) {
            QPainterPath targetPath;
            for (int x = 0; x <= w; x += 2) {
                double f = xToFreq(x, w);
                double db = m_overlay.targetCurve.evaluate(f);
                double y = dbToY(db, h);
                if (x == 0)
                    targetPath.moveTo(x, y);
                else
                    targetPath.lineTo(x, y);
            }
            painter.setPen(QPen(QColor("#ffcc00"), 1.8, Qt::DashLine));
            painter.drawPath(targetPath);
        }

        // 2. Draw Measured Response Curve (Cyan)
        if (!m_overlay.measuredMagDB.empty() && m_overlay.measuredMagDB.size() == m_overlay.frequencies.size()) {
            QPainterPath measuredPath;
            for (size_t i = 0; i < m_overlay.measuredMagDB.size(); ++i) {
                double f = m_overlay.frequencies[i];
                double db = m_overlay.measuredMagDB[i];
                double x = freqToX(f, w);
                double y = dbToY(db, h);
                if (i == 0)
                    measuredPath.moveTo(x, y);
                else
                    measuredPath.lineTo(x, y);
            }
            painter.setPen(QPen(QColor(0, 198, 255, 180), 1.2));
            painter.drawPath(measuredPath);
        }
    }

    // Individual Band Curves
    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        const auto& band = m_preset.bands[i];
        if (!band.isEnabled)
            continue;

        QPainterPath path;
        for (int x = 0; x <= w; x += 2) {
            double f = xToFreq(x, w);
            double db = band.response(f, m_sampleRate);
            double y = dbToY(db, h);
            if (x == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        QColor c = bandColor(static_cast<int>(i));
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
        if (!b.isEnabled || b.type == EQBandType::Free)
            continue;

        double handleFreq = b.freq;
        if (b.type == EQBandType::GeneralNotch)
            handleFreq = b.freqNotch;
        else if (b.type == EQBandType::LinkwitzTransform)
            handleFreq = b.freqTarget;

        double hx = freqToX(handleFreq, w);
        double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

        bool isSelected = (static_cast<int>(i) == m_selectedIndex);
        bool isHovered = (static_cast<int>(i) == m_hoveredIndex);
        QColor c = bandColor(static_cast<int>(i));

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
    int activeIndex =
        (m_draggingIndex >= 0) ? m_draggingIndex : ((m_hoveredIndex >= 0) ? m_hoveredIndex : m_selectedIndex);

    QString text;
    if (activeIndex >= 0 && activeIndex < static_cast<int>(m_preset.bands.size())) {
        const auto& b = m_preset.bands[activeIndex];
        if (b.type == EQBandType::Free) {
            text = QString("Band #%1 [Free]%7 | b0: %2 | b1: %3 | b2: %4 | a1: %5 | a2: %6")
                       .arg(activeIndex + 1)
                       .arg(b.b0, 0, 'f', 4)
                       .arg(b.b1, 0, 'f', 4)
                       .arg(b.b2, 0, 'f', 4)
                       .arg(b.a1, 0, 'f', 4)
                       .arg(b.a2, 0, 'f', 4)
                       .arg(b.isEnabled ? "" : " (OFF)");
        } else if (b.type == EQBandType::GeneralNotch) {
            text = QString("Band #%1 [GeneralNotch]%6 | Fc: %2 Hz | Fp: %3 Hz | Qp: %4 | Norm: %5")
                       .arg(activeIndex + 1)
                       .arg(static_cast<int>(std::round(b.freqNotch)))
                       .arg(static_cast<int>(std::round(b.freqPole)))
                       .arg(b.qPole, 0, 'f', 2)
                       .arg(b.normalizeAtDc ? "ON" : "OFF")
                       .arg(b.isEnabled ? "" : " (OFF)");
        } else if (b.type == EQBandType::LinkwitzTransform) {
            text = QString("Band #%1 [LinkwitzTransform]%6 | Fa: %2 Hz | Qa: %3 | Ft: %4 Hz | Qt: %5")
                       .arg(activeIndex + 1)
                       .arg(b.freqAct, 0, 'f', 1)
                       .arg(b.qAct, 0, 'f', 2)
                       .arg(b.freqTarget, 0, 'f', 1)
                       .arg(b.qTarget, 0, 'f', 2)
                       .arg(b.isEnabled ? "" : " (OFF)");
        } else {
            text = QString("Band #%1 [%2]%4 | Fc: %3 Hz")
                       .arg(activeIndex + 1)
                       .arg(QString::fromStdString(eqBandTypeToString(b.type)))
                       .arg(static_cast<int>(std::round(b.freq)))
                       .arg(b.isEnabled ? "" : " (OFF)");

            if (eqBandTypeHasGain(b.type)) {
                text += QString(" | Gain: %1%2 dB").arg(b.gain >= 0 ? "+" : "").arg(b.gain, 0, 'f', 1);
            }

            if (eqBandTypeHasQ(b.type)) {
                if (b.type == EQBandType::Lowshelf || b.type == EQBandType::Highshelf) {
                    if (b.useSlope)
                        text += QString(" | Slope: %1 dB/oct").arg(b.slope, 0, 'f', 1);
                    else
                        text += QString(" | Q: %1").arg(b.q, 0, 'f', 2);
                } else if (b.type == EQBandType::Notch || b.type == EQBandType::Bandpass ||
                           b.type == EQBandType::Allpass) {
                    if (b.useBandwidth)
                        text += QString(" | BW: %1 oct").arg(b.bandwidth, 0, 'f', 2);
                    else
                        text += QString(" | Q: %1").arg(b.q, 0, 'f', 2);
                } else {
                    text += QString(" | Q: %1").arg(b.q, 0, 'f', 2);
                }
            }
        }
    } else {
        int activeBands = 0;
        for (const auto& b : m_preset.bands)
            if (b.isEnabled)
                activeBands++;
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
    int rectX = std::max(12, w - textW - 12);
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
            if (!b.isEnabled || b.type == EQBandType::Free)
                continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch)
                handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform)
                handleFreq = b.freqTarget;

            double hx = freqToX(handleFreq, w);
            double hy = dbToY(eqBandTypeHasGain(b.type) ? b.gain : 0.0, h);

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 12.0) {
                hitIndex = static_cast<int>(i);
                break;
            }
        }

        m_draggingIndex = hitIndex;
        m_selectedIndex = hitIndex;
        if (onBandSelected)
            onBandSelected(hitIndex);
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
        db = std::max(-36.0, std::min(36.0, db));

        auto& b = m_preset.bands[m_draggingIndex];
        if (b.type == EQBandType::GeneralNotch)
            b.freqNotch = f;
        else if (b.type == EQBandType::LinkwitzTransform)
            b.freqTarget = f;
        else
            b.freq = f;

        if (eqBandTypeHasGain(b.type))
            b.gain = std::round(db * 2.0) / 2.0;

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
            if (!b.isEnabled || b.type == EQBandType::Free)
                continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch)
                handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform)
                handleFreq = b.freqTarget;

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
            if (!b.isEnabled || b.type == EQBandType::Free)
                continue;

            double handleFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch)
                handleFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform)
                handleFreq = b.freqTarget;

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
                if (band.useSlope) {
                    band.slope = std::max(0.1, std::min(20.0, band.slope * factor));
                    if (onBandQChanged)
                        onBandQChanged(targetIdx, band.slope);
                } else if (band.useBandwidth) {
                    band.bandwidth = std::max(0.1, std::min(20.0, band.bandwidth * factor));
                    if (onBandQChanged)
                        onBandQChanged(targetIdx, band.bandwidth);
                } else {
                    band.q = std::max(0.1, std::min(20.0, band.q * factor));
                    if (onBandQChanged)
                        onBandQChanged(targetIdx, band.q);
                }
                update();
            }
        }
    }
}
