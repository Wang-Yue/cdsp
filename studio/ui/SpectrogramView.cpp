#include "ui/SpectrogramView.h"

#include <QEvent>
#include <QFontDatabase>
#include <QPainterPath>
#include <cmath>

SpectrogramView::SpectrogramView(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumHeight(180);
}

SpectrogramView::SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumHeight(180);
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
    QOpenGLWidget::showEvent(event);
    if (m_engine)
        m_engine->visibilityCount++;
}

void SpectrogramView::hideEvent(QHideEvent* event) {
    QOpenGLWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0)
        m_engine->visibilityCount--;
}

void SpectrogramView::changeEvent(QEvent* event) {
    QOpenGLWidget::changeEvent(event);
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

QColor SpectrogramView::colorForDB(float db, ColorPalette palette) {
    if (std::isnan(db))
        db = -120.0f;
    float norm = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));

    QColor color;
    switch (palette) {
    case ColorPalette::Viridis: {
        static const std::vector<QColor> viridisStops = {QColor(68, 1, 84), QColor(59, 82, 139), QColor(33, 145, 140),
                                                         QColor(94, 201, 98), QColor(253, 231, 37)};
        color = interpColors(norm, viridisStops);
        break;
    }
    case ColorPalette::Magma: {
        static const std::vector<QColor> magmaStops = {QColor(0, 0, 4), QColor(81, 18, 124), QColor(182, 54, 121),
                                                       QColor(251, 136, 97), QColor(252, 253, 191)};
        color = interpColors(norm, magmaStops);
        break;
    }
    case ColorPalette::Plasma: {
        static const std::vector<QColor> plasmaStops = {QColor(13, 8, 135), QColor(126, 3, 168), QColor(204, 71, 120),
                                                        QColor(248, 149, 64), QColor(240, 249, 33)};
        color = interpColors(norm, plasmaStops);
        break;
    }
    case ColorPalette::Inferno: {
        static const std::vector<QColor> infernoStops = {QColor(0, 0, 4), QColor(87, 16, 110), QColor(187, 55, 84),
                                                         QColor(249, 142, 9), QColor(252, 255, 164)};
        color = interpColors(norm, infernoStops);
        break;
    }
    case ColorPalette::Jet: {
        static const std::vector<QColor> jetStops = {QColor(0, 0, 143), QColor(0, 222, 255), QColor(163, 255, 87),
                                                     QColor(255, 153, 0), QColor(128, 0, 0)};
        color = interpColors(norm, jetStops);
        break;
    }
    case ColorPalette::Classic:
    default: {
        static const std::vector<QColor> classicStops = {
            QColor(0, 0, 0),     // silence (-60 dB)
            QColor(0, 100, 0),   // dark green (-45 dB)
            QColor(52, 199, 89), // green (-30 dB)
            QColor(255, 204, 0), // yellow (-18 dB)
            QColor(255, 149, 0), // orange (-6 dB)
            QColor(255, 59, 48)  // red (0 dB)
        };
        color = interpColors(norm, classicStops);
        break;
    }
    }

    return color;
}

void SpectrogramView::paintGL() {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (!parentWidget() || !parentWidget()->inherits("QStackedWidget")) {
        p.fillRect(rect(), palette().color(QPalette::Base));
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
    if (count > 0) {
        QDateTime now = QDateTime::currentDateTime();
        size_t nBins = m_engine ? m_engine->nBins : 200;
        double barHeight = static_cast<double>(plotH) / static_cast<double>(std::max<size_t>(1, nBins));

        for (size_t i = 0; i < count; ++i) {
            const auto& frame = m_history[i];
            QDateTime frameTime = frame.timestamp.isValid()
                                      ? frame.timestamp
                                      : now.addMSecs(-static_cast<qint64>((count - 1 - i) * 1000 / 30));
            double timeAgo = frameTime.msecsTo(now) / 1000.0;
            if (timeAgo > 10.0 || timeAgo < 0.0)
                continue;

            double x = marginL + plotW * (1.0 - timeAgo / 10.0);

            double nextX;
            if (i < count - 1) {
                const auto& nextFrame = m_history[i + 1];
                QDateTime nextFrameTime = nextFrame.timestamp.isValid()
                                              ? nextFrame.timestamp
                                              : now.addMSecs(-static_cast<qint64>((count - 2 - i) * 1000 / 30));
                double nextTimeAgo = nextFrameTime.msecsTo(now) / 1000.0;
                nextX = marginL + plotW * (1.0 - nextTimeAgo / 10.0);
            } else {
                nextX = marginL + plotW;
            }

            double stripWidth = std::max(1.0, nextX - x);
            size_t binCount = std::min(nBins, frame.magnitudes.size());

            for (size_t j = 0; j < binCount; ++j) {
                float db = frame.magnitudes[j];
                if (std::isnan(db))
                    continue;
                float normalized = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
                if (normalized <= 0.05f)
                    continue;

                QColor color = colorForDB(db, m_palette);
                double y0 = plotH - (j + 1) * barHeight;
                double y1 = plotH - j * barHeight;
                p.fillRect(QRectF(x, y0, stripWidth, y1 - y0 + 0.5), color);
            }
        }
    }

    // Draw Frequency Y-axis labels & grid lines
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(8);
    p.setFont(monoFont);

    QColor gridPenCol = palette().color(QPalette::Mid);

    for (double target : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (target < minF || target > maxF)
            continue;
        double frac = (std::log10(target) - logMin) / (logMax - logMin);
        int y = plotH - static_cast<int>(frac * plotH);

        p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
        p.drawLine(marginL, y, w, y);

        p.setPen(palette().color(QPalette::PlaceholderText));
        QString label = target >= 1000.0 ? QString("%1k").arg(target / 1000.0) : QString("%1").arg(target);
        p.drawText(2, y + 4, label);
    }

    // Draw Time X-axis labels & vertical grid lines (0s to -10s)
    for (int sec : {0, -2, -4, -6, -8, -10}) {
        int x = marginL + plotW - static_cast<int>((-sec / 10.0) * plotW);
        p.setPen(QPen(gridPenCol, 0.5, Qt::SolidLine));
        p.drawLine(x, 0, x, plotH);

        p.setPen(palette().color(QPalette::PlaceholderText));
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
    p.setPen(palette().color(QPalette::PlaceholderText));
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
