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
    float norm = std::max(0.0f, std::min(1.0f, (db + 80.0f) / 80.0f));
    int r = 0, g = 0, b = 0;
    if (norm < 0.25f) {
        float t = norm / 0.25f;
        b = static_cast<int>(255 * t);
    } else if (norm < 0.5f) {
        float t = (norm - 0.25f) / 0.25f;
        g = static_cast<int>(255 * t);
        b = 255;
    } else if (norm < 0.75f) {
        float t = (norm - 0.5f) / 0.25f;
        g = 255;
        b = static_cast<int>(255 * (1.0f - t));
    } else {
        float t = (norm - 0.75f) / 0.25f;
        r = static_cast<int>(255 * t);
        g = static_cast<int>(255 * (1.0f - t));
    }
    return QColor(r, g, b);
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
        size_t rowCount = m_history.size();
        int rowH = std::max(2, h / static_cast<int>(rowCount));

        for (size_t r = 0; r < rowCount; ++r) {
            const auto& spec = m_history[r];
            size_t binCount = spec.magnitudes.size();
            if (binCount == 0) continue;
            int colW = std::max(1, w / static_cast<int>(binCount));

            int y = static_cast<int>(r) * rowH;
            for (size_t c = 0; c < binCount; ++c) {
                float db = spec.magnitudes[c];

                int x = static_cast<int>(c) * colW;
                p.fillRect(x, y, colW, rowH, colorForDB(db));
            }
        }
    } else {
        // 3D Isometric Landscape
        double totalDepthY = h * 0.35;
        double totalShiftX = w * 0.12;
        double plotW = w - totalShiftX;
        double plotH = h - totalDepthY;

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

            QColor sliceColor = QColor::fromHsvF(0.6f - 0.5f * (1.0f - progress), 0.8f, 0.9f);
            p.setPen(QPen(sliceColor, 1.5));
            p.drawPath(path);
        }
    }
}
