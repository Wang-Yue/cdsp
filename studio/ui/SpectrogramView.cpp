#include "ui/SpectrogramView.h"
#include "ui/StyleTheme.h"
#include <cmath>
#include <QPainterPath>

SpectrogramView::SpectrogramView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
}

SpectrogramView::SpectrogramView(std::shared_ptr<SpectrogramEngine> engine, QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(180);
    setEngine(engine);
}

void SpectrogramView::setEngine(std::shared_ptr<SpectrogramEngine> engine) {
    m_engine = engine;
    if (m_engine) {
        connect(m_engine.get(), &SpectrogramEngine::updated, this, [this]() {
            if (m_engine) setHistory(m_engine->history, m_engine->show3D);
        });
        setHistory(m_engine->history, m_engine->show3D);
    }
}

void SpectrogramView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_engine) m_engine->visibilityCount++;
}

void SpectrogramView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_engine && m_engine->visibilityCount > 0) m_engine->visibilityCount--;
}

void SpectrogramView::setHistory(const std::deque<SpectrumData>& history, bool show3D) {
    m_history = history;
    m_show3D = show3D;
    update();
}

QColor SpectrogramView::colorForDB(float db) {
    float norm = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
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

void SpectrogramView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), StyleTheme::cardBg());

    if (m_history.empty()) return;

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

        for (size_t col = 0; col < colCount; ++col) {
            const auto& spec = m_history[col];
            size_t binCount = spec.magnitudes.size();
            if (binCount == 0) continue;

            int x = marginL + plotW - static_cast<int>(col + 1) * colW;
            double logMin = std::log10(20.0), logMax = std::log10(20000.0);

            for (size_t bin = 0; bin < binCount; ++bin) {
                float freq = (bin < spec.frequencies.size()) ? spec.frequencies[bin] : (20.0f + bin * 100.0f);
                float db = spec.magnitudes[bin];

                double fracY = (std::log10(std::max(20.0f, std::min(20000.0f, freq))) - logMin) / (logMax - logMin);
                int y = plotH - static_cast<int>(fracY * plotH);
                int binH = std::max(2, plotH / static_cast<int>(binCount));

                p.fillRect(x, y - binH, colW, binH, colorForDB(db));
            }
        }

        // Draw Frequency Y-axis labels
        p.setFont(QFont("sans-serif", 9));
        p.setPen(QColor("#8e8e93"));
        p.drawText(4, 12, "20k");
        p.drawText(4, plotH / 2, "1k");
        p.drawText(4, plotH - 2, "20Hz");

        // Draw Time X-axis labels (0s to -10s)
        p.drawLine(marginL, plotH, w, plotH);
        for (int sec : {0, -2, -4, -6, -8, -10}) {
            int x = marginL + plotW - static_cast<int>((-sec / 10.0) * plotW);
            p.drawText(x - 8, h - 4, QString("%1s").arg(sec));
        }

    } else {
        // 3D Isometric CSD Landscape with solid background occlusion fill
        double totalDepthY = h * 0.35;
        double totalShiftX = w * 0.12;
        double plotW = w - totalShiftX;
        double plotH = h - totalDepthY;

        // Draw floor depth grid lines
        p.setPen(QPen(QColor(255, 255, 255, 15), 1, Qt::DashLine));
        for (int g = 0; g <= 5; ++g) {
            double prog = g / 5.0;
            double sx = totalShiftX * prog;
            double sy = totalDepthY * prog;
            p.drawLine(sx, h - sy, sx + plotW, h - sy);
        }

        // Draw depth slices back to front for proper solid occlusion
        for (int r = static_cast<int>(m_history.size()) - 1; r >= 0; --r) {
            const auto& spec = m_history[r];
            size_t count = spec.magnitudes.size();
            if (count == 0) continue;

            double progress = static_cast<double>(r) / static_cast<double>(std::max(1, static_cast<int>(m_history.size()) - 1));
            double shiftX = totalShiftX * progress;
            double shiftY = totalDepthY * progress;

            QPainterPath path;
            for (size_t c = 0; c < count; ++c) {
                float db = spec.magnitudes[c];
                float norm = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));

                double x = shiftX + (static_cast<double>(c) / static_cast<double>(count)) * plotW;
                double y = h - shiftY - norm * plotH;

                if (c == 0) path.moveTo(x, y);
                else path.lineTo(x, y);
            }

            // Closed fill path down to floor for solid background masking
            QPainterPath fillPath = path;
            fillPath.lineTo(shiftX + plotW, h - shiftY);
            fillPath.lineTo(shiftX, h - shiftY);
            fillPath.closeSubpath();

            p.fillPath(fillPath, StyleTheme::cardBg()); // Solid background occlusion fill

            QColor sliceColor = QColor::fromHsvF(0.6f - 0.5f * (1.0f - progress), 0.8f, 0.95f);
            p.setPen(QPen(sliceColor, 1.5));
            p.drawPath(path);
        }
    }
}
