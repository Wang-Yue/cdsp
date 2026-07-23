#include "ui/VisualizerDetailViews.h"

#include "ui/StyleTheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QVBoxLayout>

namespace {

QFrame* createPanelFrame(QWidget* parent) {
    auto frame = new QFrame(parent);
    bool isDark = StyleTheme::isDark();
    QString bg = isDark ? "#1c1d24" : "#f2f2f7";
    QString border = isDark ? "#2c2d3a" : "#d1d1d6";
    frame->setStyleSheet(QString("QFrame { "
                                 "   background-color: %1; "
                                 "   border-top: 1px solid %2; "
                                 "}")
                             .arg(bg, border));
    return frame;
}

QTabBar* createSegmentedPicker(const QStringList& items, QWidget* parent) {
    auto tabBar = new QTabBar(parent);
    for (const auto& item : items) {
        tabBar->addTab(item);
    }
    tabBar->setDrawBase(false);
    bool isDark = StyleTheme::isDark();
    QString bg = isDark ? "#2c2c2e" : "#e5e5ea";
    QString fg = isDark ? "#a0a5b5" : "#6c6c70";
    QString selBg = isDark ? "#007af5" : "#007aff";
    tabBar->setStyleSheet(QString("QTabBar::tab { "
                                  "   background: %1; "
                                  "   color: %2; "
                                  "   padding: 4px 12px; "
                                  "   border-radius: 6px; "
                                  "   font-weight: 600; "
                                  "   margin-right: 2px; "
                                  "} "
                                  "QTabBar::tab:selected { "
                                  "   background: %3; "
                                  "   color: #ffffff; "
                                  "}")
                              .arg(bg, fg, selBg));
    return tabBar;
}

QLabel* createCaptionLabel(const QString& text, QWidget* parent) {
    auto lbl = new QLabel(text, parent);
    bool isDark = StyleTheme::isDark();
    QString fg = isDark ? "#a0a5b5" : "#6c6c70";
    lbl->setStyleSheet(QString("font-size: 11px; font-weight: 500; color: %1;").arg(fg));
    return lbl;
}

QLabel* createHeaderTitle(const QString& text, QWidget* parent) {
    auto lbl = new QLabel(text, parent);
    lbl->setFont(QFont("sans-serif", 13, QFont::Bold));
    return lbl;
}

QPushButton* createResetButton(QWidget* parent) {
    auto btn = new QPushButton("Reset to Defaults", parent);
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

QFrame* createDivider(QWidget* parent) {
    auto divider = new QFrame(parent);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    bool isDark = StyleTheme::isDark();
    divider->setStyleSheet(QString("background-color: %1; border: none; min-height: 1px; max-height: 1px;")
                               .arg(isDark ? "#2c2d3a" : "#d1d1d6"));
    return divider;
}

} // namespace

// ==================== AnalogVUDetailView ====================

AnalogVUDetailView::AnalogVUDetailView(std::shared_ptr<MonitoringController> monitoring, QWidget* parent)
    : QWidget(parent), m_monitoring(monitoring) {
    setupUi();
    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, [this]() {
        const auto& st = m_monitoring->levelState;
        float l = !st.playbackRms.empty() ? st.playbackRms[0] : -60.0f;
        float r = st.playbackRms.size() > 1 ? st.playbackRms[1] : l;
        m_vuMeter->setLevelDB(l, r);
    });
}

void AnalogVUDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // VU Meter Display Container (padding 32)
    auto vuContainer = new QWidget(this);
    auto vuLayout = new QVBoxLayout(vuContainer);
    vuLayout->setContentsMargins(32, 32, 32, 32);

    m_vuMeter = new AnalogVUMeterView(vuContainer);
    if (m_monitoring)
        m_vuMeter->setLevelState(&m_monitoring->levelState);
    m_vuMeter->setVUSettings(m_settings);
    vuLayout->addWidget(m_vuMeter);

    mainLayout->addWidget(vuContainer, 1);

    // Divider
    mainLayout->addWidget(createDivider(this));

    // Bottom Calibration & Lighting Controls Panel
    auto panelFrame = createPanelFrame(this);
    auto panelLayout = new QVBoxLayout(panelFrame);
    panelLayout->setContentsMargins(24, 20, 24, 24);
    panelLayout->setSpacing(16);

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(createHeaderTitle("VU Calibration & Lighting", panelFrame));
    headerBox->addStretch();

    auto resetBtn = createResetButton(panelFrame);
    connect(resetBtn, &QPushButton::clicked, this, &AnalogVUDetailView::resetDefaults);
    headerBox->addWidget(resetBtn);
    panelLayout->addLayout(headerBox);

    auto grid = new QGridLayout();
    grid->setHorizontalSpacing(32);
    grid->setVerticalSpacing(16);

    auto makeSliderCell = [this, panelFrame](const QString& title, QSlider*& slider, QLabel*& label, int minVal,
                                             int maxVal, int curVal, int precision, auto onValueChanged) {
        auto cellBox = new QVBoxLayout();
        cellBox->setSpacing(4);
        cellBox->addWidget(createCaptionLabel(title, panelFrame));

        auto sliderRow = new QHBoxLayout();
        sliderRow->setSpacing(8);

        slider = new QSlider(Qt::Horizontal, panelFrame);
        slider->setRange(minVal, maxVal);
        slider->setValue(curVal);

        label = new QLabel(QString::number(curVal / (precision == 1 ? 1.0 : 100.0), 'f', precision), panelFrame);
        label->setFixedWidth(45);
        label->setFont(QFont("monospace", 11));

        connect(slider, &QSlider::valueChanged, [this, label, precision, onValueChanged](int val) {
            double value = precision == 1 ? val : (val / 100.0);
            label->setText(QString::number(value, 'f', precision));
            onValueChanged(value);
            m_vuMeter->setVUSettings(m_settings);
        });

        sliderRow->addWidget(slider);
        sliderRow->addWidget(label);
        cellBox->addLayout(sliderRow);
        return cellBox;
    };

    // Row 0: Scale Radius (1.0 .. 1.5), Pivot Position (Y) (1.0 .. 2.0)
    grid->addLayout(makeSliderCell("Scale Radius", m_radiusSlider, m_radiusLbl, 100, 150,
                                   static_cast<int>(m_settings.radiusScale * 100), 2,
                                   [this](double val) {
                                       m_settings.radiusScale = val;
                                       m_settings.save();
                                   }),
                    0, 0);

    grid->addLayout(makeSliderCell("Pivot Position (Y)", m_pivotYSlider, m_pivotYLbl, 100, 200,
                                   static_cast<int>(m_settings.pivotY * 100), 2,
                                   [this](double val) {
                                       m_settings.pivotY = val;
                                       m_settings.save();
                                   }),
                    0, 1);

    // Row 1: Needle Extension (0 .. 60), Ambient Glow (0.0 .. 1.0)
    grid->addLayout(makeSliderCell("Needle Extension", m_needleExtSlider, m_needleExtLbl, 0, 60,
                                   static_cast<int>(m_settings.needleExtension), 1,
                                   [this](double val) {
                                       m_settings.needleExtension = val;
                                       m_settings.save();
                                   }),
                    1, 0);

    grid->addLayout(makeSliderCell("Ambient Glow", m_ambientGlowSlider, m_ambientGlowLbl, 0, 100,
                                   static_cast<int>(m_settings.ambientGlow * 100), 2,
                                   [this](double val) {
                                       m_settings.ambientGlow = val;
                                       m_settings.save();
                                   }),
                    1, 1);

    // Row 2: Focused Hot Spot (0.0 .. 1.0), Overall Light Wash (0.0 .. 0.4)
    grid->addLayout(makeSliderCell("Focused Hot Spot", m_hotSpotSlider, m_hotSpotLbl, 0, 100,
                                   static_cast<int>(m_settings.hotSpotAlpha * 100), 2,
                                   [this](double val) {
                                       m_settings.hotSpotAlpha = val;
                                       m_settings.save();
                                   }),
                    2, 0);

    grid->addLayout(makeSliderCell("Overall Light Wash", m_lightWashSlider, m_lightWashLbl, 0, 40,
                                   static_cast<int>(m_settings.lightWash * 100), 2,
                                   [this](double val) {
                                       m_settings.lightWash = val;
                                       m_settings.save();
                                   }),
                    2, 1);

    panelLayout->addLayout(grid);
    mainLayout->addWidget(panelFrame);
}

void AnalogVUDetailView::resetDefaults() {
    m_settings.reset();
    m_radiusSlider->setValue(static_cast<int>(m_settings.radiusScale * 100));
    m_radiusLbl->setText(QString::number(m_settings.radiusScale, 'f', 2));
    m_pivotYSlider->setValue(static_cast<int>(m_settings.pivotY * 100));
    m_pivotYLbl->setText(QString::number(m_settings.pivotY, 'f', 2));
    m_needleExtSlider->setValue(static_cast<int>(m_settings.needleExtension));
    m_needleExtLbl->setText(QString::number(m_settings.needleExtension, 'f', 1));
    m_ambientGlowSlider->setValue(static_cast<int>(m_settings.ambientGlow * 100));
    m_ambientGlowLbl->setText(QString::number(m_settings.ambientGlow, 'f', 2));
    m_hotSpotSlider->setValue(static_cast<int>(m_settings.hotSpotAlpha * 100));
    m_hotSpotLbl->setText(QString::number(m_settings.hotSpotAlpha, 'f', 2));
    m_lightWashSlider->setValue(static_cast<int>(m_settings.lightWash * 100));
    m_lightWashLbl->setText(QString::number(m_settings.lightWash, 'f', 2));
    m_vuMeter->setVUSettings(m_settings);
}

void AnalogVUDetailView::refreshUi() {
    m_settings.load();
    m_radiusSlider->setValue(static_cast<int>(m_settings.radiusScale * 100));
    m_radiusLbl->setText(QString::number(m_settings.radiusScale, 'f', 2));
    m_pivotYSlider->setValue(static_cast<int>(m_settings.pivotY * 100));
    m_pivotYLbl->setText(QString::number(m_settings.pivotY, 'f', 2));
    m_needleExtSlider->setValue(static_cast<int>(m_settings.needleExtension));
    m_needleExtLbl->setText(QString::number(m_settings.needleExtension, 'f', 1));
    m_ambientGlowSlider->setValue(static_cast<int>(m_settings.ambientGlow * 100));
    m_ambientGlowLbl->setText(QString::number(m_settings.ambientGlow, 'f', 2));
    m_hotSpotSlider->setValue(static_cast<int>(m_settings.hotSpotAlpha * 100));
    m_hotSpotLbl->setText(QString::number(m_settings.hotSpotAlpha, 'f', 2));
    m_lightWashSlider->setValue(static_cast<int>(m_settings.lightWash * 100));
    m_lightWashLbl->setText(QString::number(m_settings.lightWash, 'f', 2));
    m_vuMeter->setVUSettings(m_settings);
}

// ==================== SpectrumDetailView ====================

SpectrumDetailView::SpectrumDetailView(std::shared_ptr<SpectrumEngine> engine,
                                       std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
    setupUi();
}

void SpectrumDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Spectrum Display Container (padding 32)
    auto spectrumContainer = new QWidget(this);
    auto spectrumLayout = new QVBoxLayout(spectrumContainer);
    spectrumLayout->setContentsMargins(32, 32, 32, 32);

    m_spectrumView = new SpectrumView(m_engine, spectrumContainer);
    spectrumLayout->addWidget(m_spectrumView);
    mainLayout->addWidget(spectrumContainer, 1);

    // Divider
    mainLayout->addWidget(createDivider(this));

    // Bottom Settings Panel
    auto panelFrame = createPanelFrame(this);
    auto panelLayout = new QVBoxLayout(panelFrame);
    panelLayout->setContentsMargins(24, 20, 24, 24);
    panelLayout->setSpacing(16);

    m_channelCombo = new QComboBox(panelFrame);

    auto updateChannelCombo = [this]() {
        m_channelCombo->blockSignals(true);
        m_channelCombo->clear();
        m_channelCombo->addItem("Average");
        int count = 8;
        if (m_devices) {
            int ch = m_engine->isCapture ? m_devices->captureConfig.channels : m_devices->playbackConfig.channels;
            if (ch > 0)
                count = std::max(2, ch);
        }
        for (int i = 0; i < count; ++i) {
            m_channelCombo->addItem(QString("Channel %1").arg(i + 1));
        }
        int curIdx = m_engine->channel.value_or(-1) + 1;
        if (curIdx < m_channelCombo->count()) {
            m_channelCombo->setCurrentIndex(curIdx);
        } else {
            m_channelCombo->setCurrentIndex(0);
            m_engine->channel.reset();
        }
        m_channelCombo->blockSignals(false);
    };

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(createHeaderTitle("Spectrum Settings", panelFrame));
    headerBox->addStretch();
    auto resetBtn = createResetButton(panelFrame);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombo]() {
        m_engine->resetToDefaults();
        m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        updateChannelCombo();
        m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
        m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);
        m_rangeLbl->setText(QString("Range: %1 - %2 Hz")
                                .arg(static_cast<int>(m_engine->minFreq))
                                .arg(static_cast<int>(m_engine->maxFreq)));
        m_spectrumView->update();
    });
    headerBox->addWidget(resetBtn);
    panelLayout->addLayout(headerBox);

    auto rowLayout = new QHBoxLayout();
    rowLayout->setSpacing(20);

    // Source (Segmented Picker)
    auto sourceBox = new QVBoxLayout();
    sourceBox->setSpacing(4);
    sourceBox->addWidget(createCaptionLabel("Source", panelFrame));
    m_sourceTabBar = createSegmentedPicker({"Capture", "Playback"}, panelFrame);
    m_sourceTabBar->setFixedWidth(140);
    m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceTabBar, &QTabBar::currentChanged, [this, updateChannelCombo](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombo();
        m_spectrumView->update();
    });
    sourceBox->addWidget(m_sourceTabBar);
    rowLayout->addLayout(sourceBox);

    // Channel
    auto channelBox = new QVBoxLayout();
    channelBox->setSpacing(4);
    channelBox->addWidget(createCaptionLabel("Channel", panelFrame));
    m_channelCombo->setFixedWidth(120);
    updateChannelCombo();
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx == 0)
            m_engine->channel.reset();
        else
            m_engine->channel = idx - 1;
        m_spectrumView->update();
    });
    channelBox->addWidget(m_channelCombo);
    rowLayout->addLayout(channelBox);

    // Bins
    auto binsBox = new QVBoxLayout();
    binsBox->setSpacing(4);
    binsBox->addWidget(createCaptionLabel("Bins", panelFrame));
    m_binsSpin = new QSpinBox(panelFrame);
    m_binsSpin->setRange(2, 100);
    m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
    m_binsSpin->setFixedWidth(100);
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nBins = static_cast<size_t>(val);
        m_spectrumView->update();
    });
    binsBox->addWidget(m_binsSpin);
    rowLayout->addLayout(binsBox);

    // Freq Range
    auto rangeBox = new QVBoxLayout();
    rangeBox->setSpacing(4);
    m_rangeLbl = createCaptionLabel(
        QString("Range: %1 - %2 Hz").arg(static_cast<int>(m_engine->minFreq)).arg(static_cast<int>(m_engine->maxFreq)),
        panelFrame);
    rangeBox->addWidget(m_rangeLbl);

    m_rangeSlider = new LogRangeSlider(panelFrame);
    m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);
    connect(m_rangeSlider, &LogRangeSlider::rangeChanged, [this](double minF, double maxF) {
        m_engine->minFreq = minF;
        m_engine->maxFreq = maxF;
        m_rangeLbl->setText(QString("Range: %1 - %2 Hz").arg(static_cast<int>(minF)).arg(static_cast<int>(maxF)));
        m_spectrumView->update();
    });
    rangeBox->addWidget(m_rangeSlider);
    rowLayout->addLayout(rangeBox, 1);

    rowLayout->addStretch();
    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelFrame);
}

// ==================== SpectrogramDetailView ====================

SpectrogramDetailView::SpectrogramDetailView(std::shared_ptr<SpectrogramEngine> engine,
                                             std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
    setupUi();
}

void SpectrogramDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Spectrogram Display Container (padding 32)
    auto specContainer = new QWidget(this);
    auto specLayout = new QVBoxLayout(specContainer);
    specLayout->setContentsMargins(32, 32, 32, 32);

    m_spectrogramView = new SpectrogramView(m_engine, specContainer);
    specLayout->addWidget(m_spectrogramView);
    mainLayout->addWidget(specContainer, 1);

    // Divider
    mainLayout->addWidget(createDivider(this));

    // Bottom Settings Panel
    auto panelFrame = createPanelFrame(this);
    auto panelLayout = new QVBoxLayout(panelFrame);
    panelLayout->setContentsMargins(24, 20, 24, 24);
    panelLayout->setSpacing(16);

    m_channelCombo = new QComboBox(panelFrame);

    auto updateChannelCombo = [this]() {
        m_channelCombo->blockSignals(true);
        m_channelCombo->clear();
        m_channelCombo->addItem("Average");
        int count = 8;
        if (m_devices) {
            int ch = m_engine->isCapture ? m_devices->captureConfig.channels : m_devices->playbackConfig.channels;
            if (ch > 0)
                count = std::max(2, ch);
        }
        for (int i = 0; i < count; ++i) {
            m_channelCombo->addItem(QString("Channel %1").arg(i + 1));
        }
        int curIdx = m_engine->channel.value_or(-1) + 1;
        if (curIdx < m_channelCombo->count()) {
            m_channelCombo->setCurrentIndex(curIdx);
        } else {
            m_channelCombo->setCurrentIndex(0);
            m_engine->channel.reset();
        }
        m_channelCombo->blockSignals(false);
    };

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(createHeaderTitle("Spectroscope Settings", panelFrame));
    headerBox->addStretch();

    auto resetBtn = createResetButton(panelFrame);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombo]() {
        m_engine->resetToDefaults();
        m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        updateChannelCombo();
        m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
        m_modeTabBar->setCurrentIndex(m_engine->show3D ? 1 : 0);
        m_spectrogramView->update();
    });
    headerBox->addWidget(resetBtn);
    panelLayout->addLayout(headerBox);

    auto rowLayout = new QHBoxLayout();
    rowLayout->setSpacing(20);

    // Source (Segmented Picker)
    auto sourceBox = new QVBoxLayout();
    sourceBox->setSpacing(4);
    sourceBox->addWidget(createCaptionLabel("Source", panelFrame));
    m_sourceTabBar = createSegmentedPicker({"Capture", "Playback"}, panelFrame);
    m_sourceTabBar->setFixedWidth(140);
    m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceTabBar, &QTabBar::currentChanged, [this, updateChannelCombo](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombo();
        m_spectrogramView->update();
    });
    sourceBox->addWidget(m_sourceTabBar);
    rowLayout->addLayout(sourceBox);

    // Channel
    auto channelBox = new QVBoxLayout();
    channelBox->setSpacing(4);
    channelBox->addWidget(createCaptionLabel("Channel", panelFrame));
    m_channelCombo->setFixedWidth(120);
    updateChannelCombo();
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx == 0)
            m_engine->channel.reset();
        else
            m_engine->channel = idx - 1;
        m_spectrogramView->update();
    });
    channelBox->addWidget(m_channelCombo);
    rowLayout->addLayout(channelBox);

    // Bins
    auto binsBox = new QVBoxLayout();
    binsBox->setSpacing(4);
    binsBox->addWidget(createCaptionLabel("Bins", panelFrame));
    m_binsSpin = new QSpinBox(panelFrame);
    m_binsSpin->setRange(20, 500);
    m_binsSpin->setSingleStep(20);
    m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
    m_binsSpin->setFixedWidth(120);
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nBins = static_cast<size_t>(val);
        m_spectrogramView->update();
    });
    binsBox->addWidget(m_binsSpin);
    rowLayout->addLayout(binsBox);

    // Display Mode (Segmented Picker)
    auto modeBox = new QVBoxLayout();
    modeBox->setSpacing(4);
    modeBox->addWidget(createCaptionLabel("Display Mode", panelFrame));
    m_modeTabBar = createSegmentedPicker({"2D Waterfall", "3D Landscape"}, panelFrame);
    m_modeTabBar->setFixedWidth(200);
    m_modeTabBar->setCurrentIndex(m_engine->show3D ? 1 : 0);
    connect(m_modeTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_engine->show3D = (idx == 1);
        m_spectrogramView->update();
    });
    modeBox->addWidget(m_modeTabBar);
    rowLayout->addLayout(modeBox);

    rowLayout->addStretch();
    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelFrame);
}

// ==================== VectorScopeDetailView ====================

VectorScopeDetailView::VectorScopeDetailView(std::shared_ptr<VectorScopeEngine> engine,
                                             std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
    setupUi();
}

void VectorScopeDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // VectorScope Display Container (padding 32)
    auto vecContainer = new QWidget(this);
    auto vecLayout = new QVBoxLayout(vecContainer);
    vecLayout->setContentsMargins(32, 32, 32, 32);

    m_vectorView = new VectorScopeView(m_engine, vecContainer);
    vecLayout->addWidget(m_vectorView);
    mainLayout->addWidget(vecContainer, 1);

    // Divider
    mainLayout->addWidget(createDivider(this));

    // Bottom Settings Panel
    auto panelFrame = createPanelFrame(this);
    auto panelLayout = new QVBoxLayout(panelFrame);
    panelLayout->setContentsMargins(24, 20, 24, 24);
    panelLayout->setSpacing(16);

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(createHeaderTitle("Vector Scope Settings", panelFrame));
    headerBox->addStretch();

    auto resetBtn = createResetButton(panelFrame);
    connect(resetBtn, &QPushButton::clicked, [this]() {
        m_engine->resetToDefaults();
        m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        m_framesSpin->setValue(static_cast<int>(m_engine->nFrames));
        m_modeTabBar->setCurrentIndex(m_engine->showParticles ? 1 : 0);
        m_autoScaleCheck->setChecked(m_engine->autoScale);
        m_vectorView->update();
    });
    headerBox->addWidget(resetBtn);
    panelLayout->addLayout(headerBox);

    auto rowLayout = new QHBoxLayout();
    rowLayout->setSpacing(20);

    // Source (Segmented Picker)
    auto sourceBox = new QVBoxLayout();
    sourceBox->setSpacing(4);
    sourceBox->addWidget(createCaptionLabel("Source", panelFrame));
    m_sourceTabBar = createSegmentedPicker({"Capture", "Playback"}, panelFrame);
    m_sourceTabBar->setFixedWidth(140);
    m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_engine->isCapture = (idx == 0);
        m_vectorView->update();
    });
    sourceBox->addWidget(m_sourceTabBar);
    rowLayout->addLayout(sourceBox);

    // Frames
    auto framesBox = new QVBoxLayout();
    framesBox->setSpacing(4);
    framesBox->addWidget(createCaptionLabel("Frames", panelFrame));
    m_framesSpin = new QSpinBox(panelFrame);
    m_framesSpin->setRange(128, 4096);
    m_framesSpin->setSingleStep(128);
    m_framesSpin->setValue(static_cast<int>(m_engine->nFrames));
    m_framesSpin->setFixedWidth(140);
    connect(m_framesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nFrames = static_cast<size_t>(val);
        m_vectorView->update();
    });
    framesBox->addWidget(m_framesSpin);
    rowLayout->addLayout(framesBox);

    // Display Mode (Segmented Picker)
    auto modeBox = new QVBoxLayout();
    modeBox->setSpacing(4);
    modeBox->addWidget(createCaptionLabel("Display Mode", panelFrame));
    m_modeTabBar = createSegmentedPicker({"Line", "Particles"}, panelFrame);
    m_modeTabBar->setFixedWidth(160);
    m_modeTabBar->setCurrentIndex(m_engine->showParticles ? 1 : 0);
    connect(m_modeTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_engine->showParticles = (idx == 1);
        m_vectorView->update();
    });
    modeBox->addWidget(m_modeTabBar);
    rowLayout->addLayout(modeBox);

    // Auto Scale Toggle
    auto autoScaleBox = new QVBoxLayout();
    autoScaleBox->setSpacing(4);
    autoScaleBox->addWidget(createCaptionLabel("Auto Scale", panelFrame));
    m_autoScaleCheck = new QCheckBox(panelFrame);
    m_autoScaleCheck->setFixedHeight(24);
    m_autoScaleCheck->setChecked(m_engine->autoScale);
    connect(m_autoScaleCheck, &QCheckBox::toggled, [this](bool chk) {
        m_engine->autoScale = chk;
        m_vectorView->update();
    });
    autoScaleBox->addWidget(m_autoScaleCheck);
    rowLayout->addLayout(autoScaleBox);

    rowLayout->addStretch();
    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelFrame);
}
