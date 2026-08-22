#include "ui/SpectrogramView.h"

#include <QEvent>
#include <QFontDatabase>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>

SpectrogramView::SpectrogramView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(40, 24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

SpectrogramView::SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent) : QWidget(parent) {
    setMinimumSize(40, 24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setEngine(engine);
}

SpectrogramView::~SpectrogramView() {
    if (isVisible() && m_engine && m_engine->visibilityCount > 0) {
        m_engine->visibilityCount--;
    }
}

void SpectrogramView::setEngine(std::shared_ptr<SpectrogramEngine> engine) {
    if (m_engine) {
        if (isVisible() && m_engine->visibilityCount > 0)
            m_engine->visibilityCount--;
        disconnect(m_engine.get(), &SpectrogramEngine::updated, this, nullptr);
    }
    m_engine = engine;
    if (m_engine) {
        if (isVisible())
            m_engine->visibilityCount++;
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

void SpectrogramView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        update();
    }
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
    if (std::isnan(t) || t <= 0.0f || stops.size() == 1)
        return stops.front();
    if (t >= 1.0f)
        return stops.back();

    float scaled = t * (stops.size() - 1);
    size_t idx = static_cast<size_t>(scaled);
    if (idx >= stops.size() - 1)
        return stops.back();
    float frac = scaled - idx;

    const QColor& c1 = stops[idx];
    const QColor& c2 = stops[idx + 1];

    int r = static_cast<int>(c1.red() + frac * (c2.red() - c1.red()));
    int g = static_cast<int>(c1.green() + frac * (c2.green() - c1.green()));
    int b = static_cast<int>(c1.blue() + frac * (c2.blue() - c1.blue()));
    int a = static_cast<int>(c1.alpha() + frac * (c2.alpha() - c1.alpha()));
    return QColor(r, g, b, a);
}

static QColor computeColorForNorm(float norm, ColorPalette palette) {
    norm = std::clamp(norm, 0.0f, 1.0f);
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
    case ColorPalette::Classic:
    default: {
        static const std::vector<QColor> classicStops = {
            QColor(0, 0, 0, 0),  // silence (-60 dB, transparent)
            QColor(0, 100, 0),   // dark green (-45 dB)
            QColor(52, 199, 89), // green (-30 dB)
            QColor(255, 204, 0), // yellow (-18 dB)
            QColor(255, 149, 0), // orange (-6 dB)
            QColor(255, 59, 48)  // red (0 dB)
        };
        return interpColors(norm, classicStops);
    }
    }
}

static const std::array<QRgb, 256>& getPaletteLUT(ColorPalette palette) {
    static const auto luts = []() {
        std::array<std::array<QRgb, 256>, 6> allLuts;
        const std::vector<ColorPalette> palettes = {ColorPalette::Classic, ColorPalette::Viridis, ColorPalette::Magma,
                                                    ColorPalette::Plasma,  ColorPalette::Inferno, ColorPalette::Jet};

        for (size_t p = 0; p < palettes.size(); ++p) {
            for (int i = 0; i < 256; ++i) {
                float norm = static_cast<float>(i) / 255.0f;
                allLuts[p][i] = computeColorForNorm(norm, palettes[p]).rgba();
            }
        }
        return allLuts;
    }();

    size_t idx = 0;
    switch (palette) {
    case ColorPalette::Classic:
        idx = 0;
        break;
    case ColorPalette::Viridis:
        idx = 1;
        break;
    case ColorPalette::Magma:
        idx = 2;
        break;
    case ColorPalette::Plasma:
        idx = 3;
        break;
    case ColorPalette::Inferno:
        idx = 4;
        break;
    case ColorPalette::Jet:
        idx = 5;
        break;
    default:
        idx = 0;
        break;
    }
    return luts[idx];
}

QColor SpectrogramView::colorForDB(float db, ColorPalette palette) {
    if (std::isnan(db))
        db = -120.0f;
    float norm = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    return computeColorForNorm(norm, palette);
}

void SpectrogramView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    bool inMiniPlayer = (parentWidget() && parentWidget()->inherits("QStackedWidget"));
    if (!inMiniPlayer) {
        p.fillRect(rect(), palette().color(QPalette::Window));
    }

    if (m_history.empty())
        return;

    int w = width();
    int h = height();

    if (!m_show3D) {
        paint2D(p, w, h);
    } else {
        paint3D(p, w, h);
    }
}

void SpectrogramView::paint2D(QPainter& p, int w, int h) {
    int marginL = 40;
    int marginB = 20;
    int plotW = w - marginL;
    int plotH = h - marginB;

    if (plotW <= 0 || plotH <= 0)
        return;

    double minF = (m_engine && m_engine->minFreq > 0) ? m_engine->minFreq : 20.0;
    double maxF = (m_engine && m_engine->maxFreq > minF) ? m_engine->maxFreq : 20000.0;
    if (minF >= maxF)
        maxF = minF + 100.0;
    double logMin = std::log10(minF), logMax = std::log10(maxF);

    size_t count = m_history.size();
    if (count > 1) {
        size_t nBins = m_engine ? m_engine->nBins : 200;
        int texW = static_cast<int>(count);
        int texH = static_cast<int>(nBins);

        if (m_textureImage.size() != QSize(texW, texH)) {
            m_textureImage = QImage(texW, texH, QImage::Format_ARGB32_Premultiplied);
        }

        const auto& lut = getPaletteLUT(m_palette);

        // Contiguous linear pixel write: ~20 microseconds
        for (int row = 0; row < texH; ++row) {
            QRgb* scanline = reinterpret_cast<QRgb*>(m_textureImage.scanLine(row));
            size_t j = texH - 1 - row; // Higher frequencies at top

            for (int col = 0; col < texW; ++col) {
                const auto& frame = m_history[col];
                float db = (j < frame.magnitudes.size()) ? frame.magnitudes[j] : -120.0f;
                if (std::isnan(db))
                    db = -120.0f;

                int idx = std::clamp(static_cast<int>((db + 60.0f) * (255.0f / 60.0f)), 0, 255);
                scanline[col] = lut[idx];
            }
        }

        // Draw hardware-accelerated textured quad across the full plot area
        size_t maxFrames = m_engine ? m_engine->maxHistory : 300;
        double fillFraction =
            std::min(1.0, static_cast<double>(count) / static_cast<double>(std::max<size_t>(1, maxFrames)));
        double drawWidth = plotW * fillFraction;
        double x0 = marginL + plotW - drawWidth;

        p.drawImage(QRectF(x0, 0, drawWidth, plotH), m_textureImage);
    }

    // Draw Frequency Y-axis labels & grid lines
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(8);
    p.setFont(monoFont);

    bool inMiniPlayer = (parentWidget() && parentWidget()->inherits("QStackedWidget"));
    QColor gridPenCol = inMiniPlayer ? QColor(255, 255, 255, 25) : palette().color(QPalette::Mid);
    QColor labelCol = inMiniPlayer ? QColor(255, 255, 255, 130) : palette().color(QPalette::PlaceholderText);

    for (double target : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (target < minF || target > maxF)
            continue;
        double frac = (std::log10(target) - logMin) / (logMax - logMin);
        int y = plotH - static_cast<int>(frac * plotH);

        p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
        p.drawLine(marginL, y, w, y);

        p.setPen(labelCol);
        QString label = target >= 1000.0 ? QString("%1k").arg(target / 1000.0) : QString("%1").arg(target);
        p.drawText(2, y + 4, label);
    }

    // Draw Time X-axis labels & vertical grid lines (0s to -10s)
    for (int sec : {0, -2, -4, -6, -8, -10}) {
        int x = marginL + plotW - static_cast<int>((-sec / 10.0) * plotW);
        p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
        p.drawLine(x, 0, x, plotH);

        p.setPen(labelCol);
        p.drawText(x - 8, h - 4, QString("%1s").arg(sec));
    }
}

void SpectrogramView::paint3D(QPainter& p, int w, int h) {
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

    size_t targetSlices = std::min<size_t>(count, 28);

    // 1. Draw 3D floor grid lines (Time Grid Lines)
    QColor gridPen = palette().color(QPalette::Mid);
    p.setPen(QPen(gridPen, 0.5));
    for (double fraction : {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}) {
        QPolygonF timeGrid;
        timeGrid.reserve(static_cast<int>(targetSlices));
        double xFlat = leftPadding + fraction * drawWidth;
        for (size_t s = 0; s < targetSlices; ++s) {
            double t = (targetSlices > 1) ? (static_cast<double>(s) / static_cast<double>(targetSlices - 1)) : 1.0;
            timeGrid.append(project(xFlat, baselineY, t));
        }
        p.drawPolyline(timeGrid);
    }

    // 2. Draw 3D floor grid lines (Frequency/Depth Grid Lines)
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        QPointF ptLeft = project(leftPadding, baselineY, t);
        QPointF ptRight = project(leftPadding + drawWidth, baselineY, t);
        p.drawLine(ptLeft, ptRight);
    }

    // 3. Draw stacked curves from back (t = 0.0, oldest) to front (t = 1.0, newest)
    double minF = (m_engine && m_engine->minFreq > 0) ? m_engine->minFreq : 20.0;
    double maxF = (m_engine && m_engine->maxFreq > minF) ? m_engine->maxFreq : 20000.0;
    if (minF >= maxF)
        maxF = minF + 100.0;
    double logMin = std::log10(minF), logMax = std::log10(maxF);

    size_t maxNBins = 0;
    for (size_t s = 0; s < targetSlices; ++s) {
        size_t i = (targetSlices > 1) ? ((s * (count - 1)) / (targetSlices - 1)) : 0;
        if (m_history[i].magnitudes.size() > maxNBins)
            maxNBins = m_history[i].magnitudes.size();
    }

    size_t drawBins = std::min(maxNBins, static_cast<size_t>(50));
    if (drawBins >= 2) {
        std::vector<double> xFlatTable(drawBins);
        std::vector<size_t> binIndexTable(drawBins);
        for (size_t k = 0; k < drawBins; ++k) {
            size_t j =
                std::min(maxNBins - 1,
                         static_cast<size_t>(std::round(static_cast<double>(k) / (drawBins - 1) * (maxNBins - 1))));
            double binFrac = static_cast<double>(j) / std::max<double>(1.0, maxNBins - 1);
            binIndexTable[k] = j;
            xFlatTable[k] = leftPadding + binFrac * drawWidth;
        }

        QPolygonF fillPoly;
        QPolygonF edgePoly;
        fillPoly.reserve(static_cast<int>(drawBins + 3));
        edgePoly.reserve(static_cast<int>(drawBins));

        QBrush fillBrush(palette().color(QPalette::Base));

        for (size_t s = 0; s < targetSlices; ++s) {
            size_t i = (targetSlices > 1) ? ((s * (count - 1)) / (targetSlices - 1)) : 0;
            const auto& frame = m_history[i];
            size_t nBins = frame.magnitudes.size();
            if (nBins < 2)
                continue;

            double t = (targetSlices > 1) ? (static_cast<double>(s) / static_cast<double>(targetSlices - 1)) : 1.0;
            QPointF startPt = project(leftPadding, baselineY, t);

            fillPoly.clear();
            edgePoly.clear();

            fillPoly.append(startPt);

            float peakDB = -120.0f;
            for (size_t k = 0; k < drawBins; ++k) {
                size_t j = binIndexTable[k];
                double xFlat = xFlatTable[k];

                float db = (j < frame.magnitudes.size()) ? frame.magnitudes[j] : -60.0f;
                if (!std::isnan(db))
                    peakDB = std::max(peakDB, db);
                float normMag = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
                double yFlat = baselineY - normMag * drawHeight;

                QPointF projPt = project(xFlat, yFlat, t);
                fillPoly.append(projPt);
                edgePoly.append(projPt);
            }

            QPointF endPt = project(leftPadding + drawWidth, baselineY, t);
            fillPoly.append(endPt);
            fillPoly.append(startPt);

            // GPU Opaque polygon fill
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(Qt::NoPen);
            p.setBrush(fillBrush);
            p.drawPolygon(fillPoly);

            // Antialiased wireframe stroke using theme palette
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(Qt::NoBrush);
            QColor curveColor = colorForDB(peakDB, m_palette);
            p.setPen(QPen(curveColor, 1.5));
            p.drawPolyline(edgePoly);
        }
    }

    // 4. Draw frequency labels at the front (t = 1.0)
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(8);
    p.setFont(monoFont);
    bool inMiniPlayer = (parentWidget() && parentWidget()->inherits("QStackedWidget"));
    QColor labelCol = inMiniPlayer ? QColor(255, 255, 255, 130) : palette().color(QPalette::PlaceholderText);
    p.setPen(labelCol);
    for (double target : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) {
        if (target < minF || target > maxF)
            continue;
        double binFrac = (std::log10(target) - logMin) / (logMax - logMin);
        double xFlat = leftPadding + binFrac * drawWidth;
        QPointF pt = project(xFlat, baselineY + 12.0, 1.0);
        QString label = target >= 1000.0 ? QString("%1k").arg(target / 1000.0) : QString("%1").arg(target);
        p.drawText(QRectF(pt.x() - 15, pt.y(), 30, 14), Qt::AlignCenter, label);
    }
}
