#include "ui/WaterfallPlotWidget.h"

#include "ui/StyleTheme.h"

#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPainterPath>
#include <QPointer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>

WaterfallPlotWidget::WaterfallPlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(280);

    setupControlsBar();

    QPointer<WaterfallPlotWidget> safeThis(this);
    connect(&m_watcher, &QFutureWatcher<std::vector<std::pair<double, FrequencyResponse>>>::finished, this,
            [safeThis]() {
                if (safeThis) {
                    safeThis->m_isComputing = false;
                    safeThis->setSlices(safeThis->m_watcher.result());
                }
            });
}

WaterfallPlotWidget::~WaterfallPlotWidget() {
    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        m_watcher.waitForFinished();
    }
}

void WaterfallPlotWidget::setupControlsBar() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    auto controlsLayout = new QHBoxLayout();
    controlsLayout->setSpacing(12);

    // 1. Time Range Combo
    controlsLayout->addWidget(new QLabel("Time Range:", this));
    m_timeCombo = new QComboBox(this);
    m_timeCombo->addItem("200 ms", 200.0);
    m_timeCombo->addItem("400 ms", 400.0);
    m_timeCombo->addItem("600 ms", 600.0);
    m_timeCombo->addItem("1000 ms", 1000.0);
    m_timeCombo->setCurrentIndex(1); // 400 ms
    connect(m_timeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_maxTimeMs = m_timeCombo->itemData(idx).toDouble();
        triggerRecompute();
    });
    controlsLayout->addWidget(m_timeCombo);

    // 2. Slices Combo
    controlsLayout->addWidget(new QLabel("Slices:", this));
    m_slicesCombo = new QComboBox(this);
    m_slicesCombo->addItem("20", 20);
    m_slicesCombo->addItem("30", 30);
    m_slicesCombo->addItem("40", 40);
    m_slicesCombo->addItem("60", 60);
    m_slicesCombo->setCurrentIndex(1); // 30
    connect(m_slicesCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_sliceCount = m_slicesCombo->itemData(idx).toInt();
        triggerRecompute();
    });
    controlsLayout->addWidget(m_slicesCombo);

    // 3. Window Length Combo
    controlsLayout->addWidget(new QLabel("Window:", this));
    m_windowCombo = new QComboBox(this);
    m_windowCombo->addItem("1024", 1024);
    m_windowCombo->addItem("2048", 2048);
    m_windowCombo->addItem("4096", 4096);
    m_windowCombo->setCurrentIndex(1); // 2048
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_windowLength = m_windowCombo->itemData(idx).toInt();
        triggerRecompute();
    });
    controlsLayout->addWidget(m_windowCombo);

    // 4. Floor dB Combo
    controlsLayout->addWidget(new QLabel("Floor:", this));
    m_floorCombo = new QComboBox(this);
    m_floorCombo->addItem("-30 dB", -30.0);
    m_floorCombo->addItem("-40 dB", -40.0);
    m_floorCombo->addItem("-50 dB", -50.0);
    m_floorCombo->addItem("-60 dB", -60.0);
    m_floorCombo->setCurrentIndex(1); // -40 dB
    connect(m_floorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_floorDB = m_floorCombo->itemData(idx).toDouble();
        update();
    });
    controlsLayout->addWidget(m_floorCombo);

    controlsLayout->addStretch();

    // 5. Export Button
    m_exportBtn = new QPushButton("Export Image…", this);
    connect(m_exportBtn, &QPushButton::clicked, this, &WaterfallPlotWidget::onExport);
    controlsLayout->addWidget(m_exportBtn);

    mainLayout->addLayout(controlsLayout);
    mainLayout->addStretch(1);
}

void WaterfallPlotWidget::setFloorDB(double floorDB) {
    m_floorDB = floorDB;
    for (int i = 0; i < m_floorCombo->count(); ++i) {
        if (std::abs(m_floorCombo->itemData(i).toDouble() - floorDB) < 0.1) {
            m_floorCombo->setCurrentIndex(i);
            break;
        }
    }
    update();
}

void WaterfallPlotWidget::setFrequencyBounds(double fMin, double fMax) {
    m_fMin = fMin;
    m_fMax = fMax;
    update();
}

void WaterfallPlotWidget::setSlices(const std::vector<std::pair<double, FrequencyResponse>>& slices) {
    m_slices = slices;
    update();
}

void WaterfallPlotWidget::triggerRecompute() {
    if (m_ir.has_value()) {
        recomputeSTFTAsync(m_ir.value(), m_sliceCount, m_maxTimeMs, m_windowLength);
    }
}

void WaterfallPlotWidget::recomputeSTFTAsync(const ImpulseResponse& ir, int sliceCount, double maxTimeMs,
                                             int windowLength) {
    m_ir = ir;
    m_sliceCount = sliceCount;
    m_maxTimeMs = maxTimeMs;
    m_windowLength = windowLength;

    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        m_watcher.waitForFinished();
    }

    m_isComputing = true;
    update();

    double tMax = maxTimeMs / 1000.0;
    int nFft = windowLength * 2;

    QFuture<std::vector<std::pair<double, FrequencyResponse>>> future =
        QtConcurrent::run([ir, sliceCount, tMax, windowLength, nFft]() {
            return FrequencyResponse::stft(ir, sliceCount, tMax, windowLength, nFft);
        });
    m_watcher.setFuture(future);
}

void WaterfallPlotWidget::onExport() {
    QString fileName = QFileDialog::getSaveFileName(this, "Export Waterfall Plot Image", "waterfall_plot.png",
                                                    "PNG Images (*.png);;JPEG Images (*.jpg)");
    if (!fileName.isEmpty()) {
        QPixmap pixmap = grab();
        if (pixmap.save(fileName)) {
            QMessageBox::information(this, "Export Successful", "Waterfall plot image saved to: " + fileName);
        } else {
            QMessageBox::warning(this, "Export Failed", "Could not save image to specified path.");
        }
    }
}

void WaterfallPlotWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int topOffset = 45;
    if (m_timeCombo && m_timeCombo->parentWidget()) {
        topOffset = m_timeCombo->geometry().bottom() + 10;
    }
    QRect plotRect = rect().adjusted(0, topOffset, 0, 0);

    p.fillRect(plotRect, StyleTheme::cardBg());

    if (m_isComputing) {
        p.setPen(StyleTheme::textSecondary());
        p.setFont(QFont("sans-serif", 12));
        p.drawText(plotRect, Qt::AlignCenter, "Computing CSD Waterfall STFT Slices…");
        return;
    }

    if (m_slices.empty()) {
        p.setPen(StyleTheme::textSecondary());
        p.setFont(QFont("sans-serif", 12));
        p.drawText(plotRect, Qt::AlignCenter, "No measurement data available to generate Waterfall.");
        return;
    }

    int w = plotRect.width();
    int h = plotRect.height();

    double totalDepthY = h * 0.4;
    double totalShiftX = w * 0.15;
    double plotW = w - totalShiftX;
    double plotH = h - totalDepthY;

    double fMin = m_fMin, fMax = m_fMax;
    double floorDB = m_floorDB;
    double logMin = std::log10(fMin);
    double logMax = std::log10(fMax);

    // Reference level (first slice peak)
    double refPeak = 0.0;
    const auto& firstFr = m_slices.front().second;
    for (size_t bin = 0; bin < firstFr.bins(); ++bin) {
        double f = firstFr.frequency(bin);
        if (f >= fMin && f <= fMax) {
            refPeak = std::max(refPeak, firstFr.magnitude(bin));
        }
    }
    double refDB = (refPeak > 0.0) ? 20.0 * std::log10(refPeak) : 0.0;

    p.save();
    p.translate(plotRect.topLeft());

    // Render back to front for isometric depth
    for (int idx = static_cast<int>(m_slices.size()) - 1; idx >= 0; --idx) {
        const auto& [timeSec, fr] = m_slices[idx];
        double progress =
            static_cast<double>(idx) / static_cast<double>(std::max(1, static_cast<int>(m_slices.size()) - 1));

        double shiftX = totalShiftX * progress;
        double shiftY = totalDepthY * progress;

        QPainterPath path;
        bool isFirst = true;

        size_t count = fr.bins();
        size_t binStride = std::max(static_cast<size_t>(1), count / 800);

        for (size_t bin = 0; bin < count; bin += binStride) {
            double f = fr.frequency(bin);
            if (f < fMin || f > fMax)
                continue;

            double mag = fr.magnitude(bin);
            double db = (mag > 0.0) ? 20.0 * std::log10(mag) - refDB : -100.0;
            double clampedDB = std::max(floorDB, std::min(10.0, db));
            double yFrac = (clampedDB - floorDB) / (10.0 - floorDB);

            double logF = std::log10(f);
            double x = shiftX + (logF - logMin) / (logMax - logMin) * plotW;
            double y = h - shiftY - yFrac * plotH;

            if (isFirst) {
                path.moveTo(x, y);
                isFirst = false;
            } else {
                path.lineTo(x, y);
            }
        }

        if (!path.isEmpty()) {
            // Fill background mask
            QPainterPath fillPath = path;
            fillPath.lineTo(shiftX + plotW, h - shiftY);
            fillPath.lineTo(shiftX, h - shiftY);
            fillPath.closeSubpath();

            QColor maskColor = StyleTheme::cardBg();
            maskColor.setAlpha(240);
            p.fillPath(fillPath, maskColor);

            double hue = 0.6 - 0.5 * (1.0 - progress);
            QColor sliceColor = QColor::fromHsvF(hue, 0.8, 0.9);
            p.setPen(QPen(sliceColor, 1.5));
            p.drawPath(path);
        }
    }

    p.restore();
}
