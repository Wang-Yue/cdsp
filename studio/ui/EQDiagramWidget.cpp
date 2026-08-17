#include "ui/EQDiagramWidget.h"

#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QMenu>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

EQDiagramWidget::EQDiagramWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
}

EQDiagramWidget::~EQDiagramWidget() {
    if (isVisible() && m_showAnalyzer && m_spectrum && m_spectrum->visibilityCount > 0) {
        m_spectrum->visibilityCount--;
    }
}

void EQDiagramWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_showAnalyzer && m_spectrum) {
        m_spectrum->visibilityCount++;
    }
}

void EQDiagramWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_showAnalyzer && m_spectrum && m_spectrum->visibilityCount > 0) {
        m_spectrum->visibilityCount--;
    }
}

void EQDiagramWidget::setSpectrumEngine(std::shared_ptr<SpectrumEngine> spectrum) {
    if (m_spectrum) {
        if (isVisible() && m_showAnalyzer && m_spectrum->visibilityCount > 0) {
            m_spectrum->visibilityCount--;
        }
        disconnect(m_spectrum.get(), &SpectrumEngine::updated, this, QOverload<>::of(&QWidget::update));
    }
    m_spectrum = spectrum;
    if (m_spectrum) {
        if (isVisible() && m_showAnalyzer) {
            m_spectrum->visibilityCount++;
        }
        if (m_showAnalyzer) {
            connect(m_spectrum.get(), &SpectrumEngine::updated, this, QOverload<>::of(&QWidget::update),
                    Qt::UniqueConnection);
        }
    }
    update();
}

void EQDiagramWidget::setShowAnalyzer(bool show) {
    if (m_showAnalyzer != show) {
        if (m_spectrum) {
            if (isVisible()) {
                if (show)
                    m_spectrum->visibilityCount++;
                else if (m_spectrum->visibilityCount > 0)
                    m_spectrum->visibilityCount--;
            }
            if (show) {
                connect(m_spectrum.get(), &SpectrumEngine::updated, this, QOverload<>::of(&QWidget::update),
                        Qt::UniqueConnection);
            } else {
                disconnect(m_spectrum.get(), &SpectrumEngine::updated, this, QOverload<>::of(&QWidget::update));
            }
        }
        m_showAnalyzer = show;
        update();
    }
}

void EQDiagramWidget::setShowLoudnessContour(bool show) {
    m_showLoudnessContour = show;
    update();
}

void EQDiagramWidget::setAudioSettings(std::shared_ptr<AudioSettings> settings) {
    m_audioSettings = settings;
    update();
}

void EQDiagramWidget::setReferenceOverlay(const EQReferenceOverlayData& overlay) {
    m_overlay = overlay;
    update();
}

void EQDiagramWidget::setPipelineStore(std::shared_ptr<PipelineStore> store) {
    m_pipelineStore = store;
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
    static const QColor colors[] = {QColor("#FF3B30"), QColor("#FF9500"), QColor("#FFCC00"), QColor("#34C759"),
                                    QColor("#32ADE6"), QColor("#007AFF"), QColor("#AF52DE"), QColor("#FF2D55"),
                                    QColor("#00C7BE"), QColor("#30B0C7"), QColor("#5856D6"), QColor("#A2845E")};
    return colors[std::abs(index) % 12];
}

double EQDiagramWidget::freqToX(double f, double width) const {
    if (width <= 0.0)
        return 0.0;
    double minLog = std::log10(std::max(1.0, fMin));
    double maxLog = std::log10(std::max(fMin + 1.0, fMax));
    double logF = std::log10(std::max(fMin, f));
    return width * (logF - minLog) / (maxLog - minLog);
}

double EQDiagramWidget::xToFreq(double x, double width) const {
    double minLog = std::log10(std::max(1.0, fMin));
    double maxLog = std::log10(std::max(fMin + 1.0, fMax));
    double ratio = std::max(0.0, std::min(1.0, width > 0.0 ? (x / width) : 0.0));
    return std::pow(10.0, minLog + ratio * (maxLog - minLog));
}

double EQDiagramWidget::dbToY(double db, double height) const {
    if (height <= 0.0)
        return 0.0;
    double denom = (dbMax > dbMin) ? (dbMax - dbMin) : 1.0;
    double ratio = (db - dbMin) / denom;
    return height * (1.0 - ratio);
}

double EQDiagramWidget::yToDb(double y, double height) const {
    double ratio = 1.0 - (height > 0.0 ? (y / height) : 0.0);
    return dbMin + ratio * (dbMax - dbMin);
}

void EQDiagramWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    int sampleRate = (m_sampleRate > 0) ? m_sampleRate : 48000;

    // Card background
    painter.fillRect(rect(), palette().color(QPalette::Base));

    // 1. Live Spectrum Analyzer Background Overlay
    if (m_showAnalyzer && m_spectrum && !m_spectrum->data.magnitudes.empty()) {
        const auto& specBands = m_spectrum->data.magnitudes;
        const auto& specFreqs = m_spectrum->data.frequencies;
        if (!specBands.empty() && specBands.size() == specFreqs.size()) {
            double peakDB = -120.0;
            double sumDB = 0.0;
            size_t validCount = 0;
            for (float val : specBands) {
                if (std::isnan(val))
                    continue;
                double v = static_cast<double>(val);
                peakDB = std::max(peakDB, v);
                sumDB += v;
                validCount++;
            }
            double offset = (peakDB < -95.0 || validCount == 0) ? 0.0 : (sumDB / static_cast<double>(validCount));

            QPainterPath fillPath, strokePath;
            fillPath.moveTo(0, h);

            for (size_t i = 0; i < specBands.size(); ++i) {
                double f = specFreqs[i];
                float rawVal = specBands[i];
                double rawDb = std::isnan(rawVal) ? -120.0 : static_cast<double>(rawVal);
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
    QFont gridMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    gridMono.setPointSize(9);
    painter.setFont(gridMono);
    QColor gridPenColor = palette().color(QPalette::Mid);
    for (double db = -18.0; db <= 18.0; db += 6.0) {
        if (db == 0.0)
            continue; // 0 dB line drawn separately
        double y = dbToY(db, h);
        painter.setPen(QPen(gridPenColor, 0.5, Qt::SolidLine));
        painter.drawLine(0, y, w, y);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(28, y - 4, QString("%1 dB").arg(static_cast<int>(db)));
    }
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        double x = freqToX(f, w);
        painter.setPen(QPen(gridPenColor, 0.5, Qt::SolidLine));
        painter.drawLine(x, 0, x, h);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QString label = (f >= 1000.0) ? QString("%1k").arg(f / 1000.0) : QString("%1").arg(f);
        painter.drawText(x - 10, h - 8, label);
    }

    // 0 dB Baseline
    QColor zeroDbPenColor = palette().color(QPalette::Mid);
    painter.setPen(QPen(zeroDbPenColor, 1.0, Qt::SolidLine));
    double zeroY = dbToY(0.0, h);
    painter.drawLine(0, zeroY, w, zeroY);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(28, zeroY - 4, "0 dB");

    // Reference Target & Measured Frequency Response Curves Overlay
    if (m_overlay.active) {
        // Compute 200 Hz - 5000 Hz median offset (normDB)
        double normDB = 0.0;
        if (!m_overlay.measuredMagDB.empty() && m_overlay.measuredMagDB.size() == m_overlay.frequencies.size()) {
            std::vector<double> inBand;
            inBand.reserve(m_overlay.measuredMagDB.size());
            for (size_t i = 0; i < m_overlay.frequencies.size(); ++i) {
                double f = m_overlay.frequencies[i];
                double m = m_overlay.measuredMagDB[i];
                if (f >= 200.0 && f <= 5000.0 && std::isfinite(m) && m > -200.0) {
                    inBand.push_back(m);
                }
            }
            if (!inBand.empty()) {
                std::sort(inBand.begin(), inBand.end());
                normDB = inBand[inBand.size() / 2];
            }
        }

        // 1. Target Curve (Dashed Secondary Gray)
        if (!m_overlay.targetCurve.breakpoints.empty()) {
            QPainterPath targetPath;
            for (int x = 0; x <= w; x += 2) {
                double f = xToFreq(x, w);
                double db = std::max(-30.0, std::min(30.0, m_overlay.targetCurve.evaluate(f)));
                double y = dbToY(db, h);
                if (x == 0)
                    targetPath.moveTo(x, y);
                else
                    targetPath.lineTo(x, y);
            }
            QPen targetPen(palette().color(QPalette::PlaceholderText), 1.2);
            targetPen.setDashPattern({4, 3});
            painter.setPen(targetPen);
            painter.drawPath(targetPath);
        }

        // 2. Measured Response Curve (Blue, Normalized by normDB)
        if (!m_overlay.measuredMagDB.empty() && m_overlay.measuredMagDB.size() == m_overlay.frequencies.size()) {
            QPainterPath measuredPath;
            for (size_t i = 0; i < m_overlay.measuredMagDB.size(); ++i) {
                double f = m_overlay.frequencies[i];
                double db = std::max(-30.0, std::min(30.0, m_overlay.measuredMagDB[i] - normDB));
                double x = freqToX(f, w);
                double y = dbToY(db, h);
                if (i == 0)
                    measuredPath.moveTo(x, y);
                else
                    measuredPath.lineTo(x, y);
            }
            painter.setPen(QPen(QColor("#007AFF"), 1.4));
            painter.drawPath(measuredPath);

            // 3. Corrected Response Curve (Orange)
            if (m_overlay.showCorrected) {
                QPainterPath correctedPath;
                for (size_t i = 0; i < m_overlay.frequencies.size(); ++i) {
                    double f = m_overlay.frequencies[i];
                    double db = std::max(-30.0, std::min(30.0, m_overlay.measuredMagDB[i] - normDB +
                                                                   m_preset.combinedResponse(f, sampleRate)));
                    double x = freqToX(f, w);
                    double y = dbToY(db, h);
                    if (i == 0)
                        correctedPath.moveTo(x, y);
                    else
                        correctedPath.lineTo(x, y);
                }
                painter.setPen(QPen(QColor("#ff9500"), 1.8));
                painter.drawPath(correctedPath);
            }
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
            double db = band.response(f, sampleRate);
            double y = dbToY(db, h);
            if (x == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        QColor c = bandColor(static_cast<int>(i));
        bool isSelected = (static_cast<int>(i) == m_selectedIndex);
        bool isHovered = (static_cast<int>(i) == m_hoveredIndex);
        painter.setPen(QPen(c, isSelected ? 2.0 : (isHovered ? 1.5 : 1.0)));
        painter.drawPath(path);
    }

    // Equal-Loudness Contour Reference Curve Overlay (after individual band curves)
    if (m_showLoudnessContour) {
        double ref = 0.0;
        double lowBoost = 0.0;
        double highBoost = 0.0;

        if (m_pipelineStore) {
            for (const auto& stage : m_pipelineStore->stages) {
                if (stage.type == StageType::Loudness && stage.isEnabled) {
                    ref = stage.loudnessReference;
                    lowBoost = stage.loudnessLowBoost;
                    highBoost = stage.loudnessHighBoost;
                    break;
                }
            }
        }

        double volumeDb = m_audioSettings ? static_cast<double>(m_audioSettings->volume) : 0.0;
        double A = std::max(0.0, ref - volumeDb);
        double scaleRange = ref - (-60.0);
        double factor = (scaleRange > 0.1) ? std::min(1.0, A / scaleRange) : 0.0;

        double bassGain = lowBoost * factor;
        double trebleGain = highBoost * factor;

        QPainterPath loudnessPath;
        for (int x = 0; x <= w; x += 2) {
            double f = xToFreq(x, w);
            double bassLoss = bassGain * (1.0 / (1.0 + std::pow(f / 130.0, 2.0)));
            double trebleLoss = trebleGain * (std::pow(f / 5000.0, 2.0) / (1.0 + std::pow(f / 5000.0, 2.0)));
            double db = std::max(-30.0, std::min(30.0, bassLoss + trebleLoss));
            double y = dbToY(db, h);
            if (x == 0)
                loudnessPath.moveTo(x, y);
            else
                loudnessPath.lineTo(x, y);
        }
        QPen loudnessPen(QColor(255, 149, 0, 166), 1.5);
        loudnessPen.setDashPattern({4, 4});
        painter.setPen(loudnessPen);
        painter.drawPath(loudnessPath);
    }

    // Combined Response Curve Line Stroke (no fill)
    QPainterPath totalPath;
    for (int x = 0; x <= w; x += 2) {
        double f = xToFreq(x, w);
        double db = m_preset.combinedResponse(f, sampleRate);
        double y = dbToY(db, h);
        if (x == 0) {
            totalPath.moveTo(x, y);
        } else {
            totalPath.lineTo(x, y);
        }
    }
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2.5));
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
    int maxCardW = std::max(60, w - 24);
    int cardW = std::min(textW, maxCardW);
    int textH = 24;
    int rectX = std::max(12, w - cardW - 12);
    int rectY = 10;

    QRect bgRect(rectX, rectY, cardW, textH);
    painter.setBrush(QColor(20, 20, 25, 200));
    painter.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    painter.drawRoundedRect(bgRect, 6, 6);

    painter.setPen(QColor(240, 240, 245));
    QString displayText = (cardW < textW) ? fm.elidedText(text, Qt::ElideMiddle, cardW - 10) : text;
    painter.drawText(bgRect, Qt::AlignCenter, displayText);
}

void EQDiagramWidget::contextMenuEvent(QContextMenuEvent* event) {
    int w = width();
    int h = height();
    QPoint pos = event->pos();

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

        if (std::hypot(pos.x() - hx, pos.y() - hy) <= 24.0) {
            hitIndex = static_cast<int>(i);
            break;
        }
    }

    QMenu menu(this);
    if (hitIndex >= 0) {
        auto& band = m_preset.bands[hitIndex];
        m_selectedIndex = hitIndex;
        if (onBandSelected)
            onBandSelected(hitIndex);
        update();

        auto toggleAct = menu.addAction(band.isEnabled ? "Disable Band" : "Enable Band");
        connect(toggleAct, &QAction::triggered, [this, hitIndex]() {
            if (hitIndex >= 0 && hitIndex < static_cast<int>(m_preset.bands.size())) {
                m_preset.bands[hitIndex].isEnabled = !m_preset.bands[hitIndex].isEnabled;
                if (onPresetChanged)
                    onPresetChanged();
                update();
            }
        });

        auto typeMenu = menu.addMenu("Change Type");
        for (EQBandType t :
             {EQBandType::Peaking, EQBandType::Lowshelf, EQBandType::Highshelf, EQBandType::Lowpass,
              EQBandType::Highpass, EQBandType::LowpassFO, EQBandType::HighpassFO, EQBandType::LowshelfFO,
              EQBandType::HighshelfFO, EQBandType::Notch, EQBandType::Bandpass, EQBandType::Allpass,
              EQBandType::AllpassFO, EQBandType::Free, EQBandType::GeneralNotch, EQBandType::LinkwitzTransform}) {
            auto act = typeMenu->addAction(QString::fromStdString(eqBandTypeToString(t)));
            connect(act, &QAction::triggered, [this, hitIndex, t]() {
                if (hitIndex >= 0 && hitIndex < static_cast<int>(m_preset.bands.size())) {
                    m_preset.bands[hitIndex].type = t;
                    if (onPresetChanged)
                        onPresetChanged();
                    update();
                }
            });
        }

        menu.addSeparator();
        auto deleteAct = menu.addAction("Delete Band");
        connect(deleteAct, &QAction::triggered, [this, hitIndex]() {
            if (hitIndex < static_cast<int>(m_preset.bands.size())) {
                m_preset.bands.erase(m_preset.bands.begin() + hitIndex);
                m_selectedIndex = -1;
                if (onBandDeleted)
                    onBandDeleted(hitIndex);
                if (onPresetChanged)
                    onPresetChanged();
                update();
            }
        });
    } else {
        double f = std::max(20.0, std::min(20000.0, xToFreq(pos.x(), w)));
        double db = std::max(-20.0, std::min(20.0, std::round(yToDb(pos.y(), h) * 2.0) / 2.0));

        auto addAct = menu.addAction(
            QString("Add Filter at %1 Hz (%2 dB)").arg(static_cast<int>(std::round(f))).arg(db, 0, 'f', 1));
        connect(addAct, &QAction::triggered, [this, f, db]() {
            EQBand newBand;
            newBand.type = EQBandType::Peaking;
            newBand.freq = f;
            newBand.gain = db;
            newBand.q = 1.414;
            newBand.isEnabled = true;
            m_preset.bands.push_back(newBand);
            m_selectedIndex = static_cast<int>(m_preset.bands.size()) - 1;
            if (onBandAdded)
                onBandAdded(f, db);
            if (onPresetChanged)
                onPresetChanged();
            update();
        });
    }
    menu.exec(event->globalPos());
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

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 24.0) {
                hitIndex = static_cast<int>(i);
                break;
            }
        }

        m_draggingIndex = hitIndex;
        m_selectedIndex = hitIndex;
        m_lastDragY = static_cast<int>(event->position().y());
        if (onBandSelected)
            onBandSelected(hitIndex);
        update();
    }
}

void EQDiagramWidget::mouseMoveEvent(QMouseEvent* event) {
    int w = width();
    int h = height();

    if (m_draggingIndex >= 0 && m_draggingIndex < static_cast<int>(m_preset.bands.size())) {
        auto& b = m_preset.bands[m_draggingIndex];

        if (event->modifiers() & Qt::ShiftModifier) {
            // Shift-Drag Q Tuning based on mouse vertical movement
            int currentY = static_cast<int>(event->position().y());
            int dy = currentY - m_lastDragY;
            if (dy != 0 && eqBandTypeHasQ(b.type)) {
                double factor = std::pow(1.01, -dy);
                if (b.useSlope) {
                    b.slope = std::max(0.1, std::min(20.0, b.slope * factor));
                    if (onBandQChanged)
                        onBandQChanged(m_draggingIndex, b.slope);
                } else if (b.useBandwidth) {
                    b.bandwidth = std::max(0.1, std::min(20.0, b.bandwidth / factor));
                    if (onBandQChanged)
                        onBandQChanged(m_draggingIndex, b.bandwidth);
                } else {
                    b.q = std::max(0.1, std::min(20.0, b.q * factor));
                    if (onBandQChanged)
                        onBandQChanged(m_draggingIndex, b.q);
                }
            }
            m_lastDragY = currentY;
        } else {
            m_lastDragY = static_cast<int>(event->position().y());
            double f = xToFreq(event->position().x(), w);
            double db = yToDb(event->position().y(), h);

            f = std::max(20.0, std::min(20000.0, f));
            db = std::max(-20.0, std::min(20.0, db));

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

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 24.0) {
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

            if (std::hypot(event->position().x() - hx, event->position().y() - hy) <= 24.0) {
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
                    band.bandwidth = std::max(0.1, std::min(20.0, band.bandwidth / factor));
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
