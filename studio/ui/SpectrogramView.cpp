#include "ui/SpectrogramView.h"

#include "ui/StyleTheme.h"

#include <QFontDatabase>
#include <QPainterPath>
#include <cmath>

SpectrogramView::SpectrogramView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

SpectrogramView::SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setEngine(engine);
}

void SpectrogramView::setEngine(std::shared_ptr<SpectrogramEngine> engine) {
    m_engine = engine;
    if (m_engine) {
        connect(m_engine.get(), &SpectrogramEngine::updated, this, [this]() {
            if (m_engine)
                setHistory(m_engine->history, m_engine->show3D, m_engine->colorPalette);
        });
        setHistory(m_engine->history, m_engine->show3D, m_engine->colorPalette);
    }
}

void SpectrogramView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_engine)
        m_engine->visibilityCount++;
}

void SpectrogramView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0)
        m_engine->visibilityCount--;
}

void SpectrogramView::setHistory(const std::deque<SpectrumData>& history, bool show3D, ColorPalette palette) {
    m_history = history;
    m_show3D = show3D;
    m_palette = palette;
    update();
}

static QColor interpColors(float t, const std::vector<QColor>& stops) {
    if (stops.empty())
        return QColor(0, 0, 0);
    if (stops.size() == 1 || t <= 0.0f)
        return stops.front();
    if (t >= 1.0f)
        return stops.back();

    float scaled = t * (stops.size() - 1);
    size_t idx = static_cast<size_t>(scaled);
    float frac = scaled - idx;

    const QColor& c1 = stops[idx];
    const QColor& c2 = stops[idx + 1];

    int r = static_cast<int>(c1.red() + frac * (c2.red() - c1.red()));
    int g = static_cast<int>(c1.green() + frac * (c2.green() - c1.green()));
    int b = static_cast<int>(c1.blue() + frac * (c2.blue() - c1.blue()));
    int a = static_cast<int>(c1.alpha() + frac * (c2.alpha() - c1.alpha()));
    return QColor(r, g, b, a);
}

QColor SpectrogramView::colorForDB(float db, ColorPalette palette) {
    float norm = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));

    switch (palette) {
    case ColorPalette::Viridis: {
        static const std::vector<QColor> viridisStops = {QColor(68, 1, 84), QColor(59, 82, 139), QColor(33, 145, 140),
                                                         QColor(94, 201, 98), QColor(253, 231, 37)};
        return interpColors(norm, viridisStops);
    }
    case ColorPalette::Magma: {
        static const std::vector<QColor> magmaStops = {QColor(0, 0, 4), QColor(81, 18, 124), QColor(182, 54, 121),
                                                       QColor(251, 136, 97), QColor(252, 253, 191)};
        return interpColors(norm, magmaStops);
    }
    case ColorPalette::Plasma: {
        static const std::vector<QColor> plasmaStops = {QColor(13, 8, 135), QColor(126, 3, 168), QColor(204, 71, 120),
                                                        QColor(248, 149, 64), QColor(240, 249, 33)};
        return interpColors(norm, plasmaStops);
    }
    case ColorPalette::Inferno: {
        static const std::vector<QColor> infernoStops = {QColor(0, 0, 4), QColor(87, 16, 110), QColor(187, 55, 84),
                                                         QColor(249, 142, 9), QColor(252, 255, 164)};
        return interpColors(norm, infernoStops);
    }
    case ColorPalette::Jet: {
        static const std::vector<QColor> jetStops = {QColor(0, 0, 143), QColor(0, 222, 255), QColor(163, 255, 87),
                                                     QColor(255, 153, 0), QColor(128, 0, 0)};
        return interpColors(norm, jetStops);
    }
    case ColorPalette::Default:
    default: {
        if (norm < 0.35f) {
            return QColor(52, 199, 89, static_cast<int>(255 * norm / 0.35f));
        } else if (norm < 0.55f) {
            float t = (norm - 0.35f) / 0.2f;
            return QColor(static_cast<int>(255 * t), 204, 0);
        } else if (norm < 0.75f) {
            float t = (norm - 0.55f) / 0.2f;
            return QColor(255, static_cast<int>(149 + 56 * (1.0f - t)), 0);
        } else {
            float t = (norm - 0.75f) / 0.25f;
            return QColor(255, static_cast<int>(59 * (1.0f - t)), 50);
        }
    }
    }
}

void SpectrogramView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    if (m_history.empty())
        return;

    int w = width();
    int h = height();

    if (!m_show3D) {
        // 2D Mode: Vertical Y-axis log-frequency, horizontal X-axis rolling time (0s to -10s)
        int marginL = 40;
        int marginB = 20;
        int plotW = w - marginL;
        int plotH = h - marginB;

        size_t colCount = m_history.size();
        int colW = std::max(2, plotW / static_cast<int>(colCount));

        double logMin = std::log10(20.0), logMax = std::log10(20000.0);

        for (size_t col = 0; col < colCount; ++col) {
            const auto& spec = m_history[col];
            size_t binCount = spec.magnitudes.size();
            if (binCount == 0)
                continue;

            int x = marginL + plotW - static_cast<int>(col + 1) * colW;

            for (size_t bin = 0; bin < binCount; ++bin) {
                float freqLower =
                    (bin < spec.frequencies.size())
                        ? spec.frequencies[bin]
                        : static_cast<float>(20.0 * std::pow(1000.0, static_cast<double>(bin) / binCount));
                float freqUpper;
                if (bin + 1 < spec.frequencies.size()) {
                    freqUpper = spec.frequencies[bin + 1];
                } else if (bin < spec.frequencies.size() && bin > 0) {
                    freqUpper = spec.frequencies[bin] + (spec.frequencies[bin] - spec.frequencies[bin - 1]);
                } else {
                    freqUpper = static_cast<float>(20.0 * std::pow(1000.0, static_cast<double>(bin + 1) / binCount));
                }

                freqLower = std::max(20.0f, std::min(20000.0f, freqLower));
                freqUpper = std::max(20.0f, std::min(20000.0f, freqUpper));

                double fracYBot = (std::log10(freqLower) - logMin) / (logMax - logMin);
                double fracYTop = (std::log10(freqUpper) - logMin) / (logMax - logMin);

                int yBottom = plotH - static_cast<int>(fracYBot * plotH);
                int yTop = plotH - static_cast<int>(fracYTop * plotH);
                int binH = std::max(1, yBottom - yTop);

                float db = spec.magnitudes[bin];
                p.fillRect(x, yTop, colW, binH, colorForDB(db, m_palette));
            }
        }

        // Draw Frequency Y-axis labels & grid lines (20, 50, 100, 200, 500, 1k, 2k, 5k, 10k, 20k)
        QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        monoFont.setPointSize(8);
        p.setFont(monoFont);

        QColor gridPenCol = StyleTheme::isDark() ? QColor(255, 255, 255, 13) : QColor(0, 0, 0, 13);

        for (double target : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
            double frac = (std::log10(target) - logMin) / (logMax - logMin);
            int y = plotH - static_cast<int>(frac * plotH);

            p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
            p.drawLine(marginL, y, w, y);

            p.setPen(StyleTheme::textSecondary());
            QString label = target >= 1000.0 ? QString("%1k").arg(target / 1000.0) : QString("%1").arg(target);
            p.drawText(2, y + 4, label);
        }

        // Draw Time X-axis labels & vertical grid lines (0s to -10s)
        for (int sec : {0, -2, -4, -6, -8, -10}) {
            int x = marginL + plotW - static_cast<int>((-sec / 10.0) * plotW);
            p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
            p.drawLine(x, 0, x, plotH);

            p.setPen(StyleTheme::textSecondary());
            p.drawText(x - 8, h - 4, QString("%1s").arg(sec));
        }

    } else {
        // 3D CSD Waterfall Landscape matching SwiftUI CSDWaterfallView
        size_t count = m_history.size();
        if (count < 2)
            return;

        double maxShiftX = w * 0.12;
        double maxShiftY = -h * 0.18;
        double maxScale = 0.85;

        double baselineY = h * 0.88;
        double drawWidth = w * 0.82;
        double drawHeight = h * 0.58;
        double leftPadding = w * 0.05;

        auto project = [&](double flatX, double flatY, double t) -> QPointF {
            double shiftX = (1.0 - t) * maxShiftX;
            double shiftY = (1.0 - t) * maxShiftY;
            double scale = maxScale + (1.0 - maxScale) * t;
            QPointF currentCenter(leftPadding + drawWidth / 2.0, baselineY);
            double px = currentCenter.x() + (flatX - currentCenter.x()) * scale + shiftX;
            double py = currentCenter.y() + (flatY - currentCenter.y()) * scale + shiftY;
            return QPointF(px, py);
        };

        // 1. Draw 3D floor grid lines (Time Grid Lines)
        p.setPen(QPen(StyleTheme::gridPenColor(), 1));
        for (double fraction : {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}) {
            QPainterPath timeGridPath;
            double xFlat = leftPadding + fraction * drawWidth;
            timeGridPath.moveTo(project(xFlat, baselineY, 0.0));
            for (size_t i = 1; i < count; ++i) {
                double t = static_cast<double>(i) / static_cast<double>(count - 1);
                timeGridPath.lineTo(project(xFlat, baselineY, t));
            }
            p.drawPath(timeGridPath);
        }

        // 2. Draw 3D floor grid lines (Frequency/Depth Grid Lines)
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            QPointF ptLeft = project(leftPadding, baselineY, t);
            QPointF ptRight = project(leftPadding + drawWidth, baselineY, t);
            p.drawLine(ptLeft, ptRight);
        }

        // 3. Draw stacked curves from back (t = 0.0, oldest) to front (t = 1.0, newest)
        double logMin = std::log10(20.0), logMax = std::log10(20000.0);

        for (size_t i = 0; i < count; ++i) {
            const auto& frame = m_history[i];
            size_t nBins = frame.magnitudes.size();
            if (nBins < 2)
                continue;

            double t = static_cast<double>(i) / static_cast<double>(count - 1);
            QPointF startPt = project(leftPadding, baselineY, t);

            // Build closed filled shape path for background occlusion
            QPainterPath fillPath;
            fillPath.moveTo(startPt);

            QPainterPath edgePath;
            bool firstEdge = true;

            size_t drawBins = std::min(nBins, static_cast<size_t>(100));
            if (drawBins < 2)
                continue;

            float maxDbInFrame = -60.0f;

            for (size_t k = 0; k < drawBins; ++k) {
                size_t j = std::min(
                    nBins - 1, static_cast<size_t>(std::round(static_cast<double>(k) / (drawBins - 1) * (nBins - 1))));
                float freq = (j < frame.frequencies.size())
                                 ? frame.frequencies[j]
                                 : static_cast<float>(
                                       20.0 * std::pow(1000.0, static_cast<double>(j) /
                                                                   std::max(1.0, static_cast<double>(nBins - 1))));
                freq = std::max(20.0f, std::min(20000.0f, freq));

                double binFrac = (std::log10(freq) - logMin) / (logMax - logMin);
                binFrac = std::max(0.0, std::min(1.0, binFrac));
                double xFlat = leftPadding + binFrac * drawWidth;

                float db = (j < frame.magnitudes.size()) ? frame.magnitudes[j] : -60.0f;
                maxDbInFrame = std::max(maxDbInFrame, db);

                float normMag = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
                double yFlat = baselineY - normMag * drawHeight;

                QPointF projPt = project(xFlat, yFlat, t);
                if (!std::isfinite(projPt.x()) || !std::isfinite(projPt.y()))
                    continue;

                fillPath.lineTo(projPt);

                if (firstEdge) {
                    edgePath.moveTo(projPt);
                    firstEdge = false;
                } else {
                    edgePath.lineTo(projPt);
                }
            }

            QPointF endPt = project(leftPadding + drawWidth, baselineY, t);
            if (std::isfinite(endPt.x()) && std::isfinite(endPt.y()) && std::isfinite(startPt.x()) &&
                std::isfinite(startPt.y())) {
                fillPath.lineTo(endPt);
                fillPath.lineTo(startPt);
            }

            // Occlusion fill
            p.fillPath(fillPath, StyleTheme::cardBg());

            // Smooth log-frequency & depth age combined color mapping
            float tf = static_cast<float>(t);
            QColor startColor(0, 122, 255, 76); // Blue 30%
            QColor endColor(0, 122, 255, 255);  // Accent Blue 100%
            int r = std::lrint(startColor.red() + tf * (endColor.red() - startColor.red()));
            int g = std::lrint(startColor.green() + tf * (endColor.green() - startColor.green()));
            int b = std::lrint(startColor.blue() + tf * (endColor.blue() - startColor.blue()));
            int a = std::lrint(startColor.alpha() + tf * (endColor.alpha() - startColor.alpha()));

            p.setPen(QPen(QColor(r, g, b, a), 1.5));
            p.drawPath(edgePath);
        }

        // 4. Draw frequency labels at the front (t = 1.0)
        p.setFont(QFont("sans-serif", 8));
        p.setPen(StyleTheme::textSecondary());
        for (double target : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) {
            double binFrac = (std::log10(target) - logMin) / (logMax - logMin);
            double xFlat = leftPadding + binFrac * drawWidth;
            QPointF pt = project(xFlat, baselineY + 12.0, 1.0);
            QString label = target >= 1000.0 ? QString("%1k").arg(target / 1000.0) : QString("%1").arg(target);
            p.drawText(QRectF(pt.x() - 15, pt.y(), 30, 14), Qt::AlignCenter, label);
        }
    }
}
