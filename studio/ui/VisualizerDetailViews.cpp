#include "ui/VisualizerDetailViews.h"

#include "ui/StyleTheme.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

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
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // Top Card: Header & Theme Selector
    auto topCard = new QWidget(this);
    auto topLayout = new QHBoxLayout(topCard);
    topLayout->setContentsMargins(8, 8, 8, 8);

    auto titleLbl = new QLabel("Analog VU Meter", topCard);
    titleLbl->setFont(QFont("sans-serif", 13, QFont::Bold));
    topLayout->addWidget(titleLbl);
    topLayout->addStretch();

    topLayout->addWidget(new QLabel("Theme:", topCard));
    m_themeCombo = new QComboBox(topCard);
    m_themeCombo->addItems({"Vintage Amber", "Dark Stealth", "Warm Tube"});
    m_themeCombo->setCurrentIndex(static_cast<int>(m_settings.theme));
    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_settings.theme = static_cast<VUTheme>(idx);
        m_settings.save();
        m_vuMeter->setVUSettings(m_settings);
    });
    topLayout->addWidget(m_themeCombo);

    mainLayout->addWidget(topCard);

    // VU Meter Center Display
    m_vuMeter = new AnalogVUMeterView(this);
    if (m_monitoring)
        m_vuMeter->setLevelState(&m_monitoring->levelState);
    m_vuMeter->setVUSettings(m_settings);
    mainLayout->addWidget(m_vuMeter, 1);

    // Bottom Calibration & Lighting Controls Panel
    auto calibGroup = new QGroupBox("VU Calibration & Lighting Panel", this);
    auto calibLayout = new QVBoxLayout(calibGroup);

    auto headerBox = new QHBoxLayout();
    auto iconLbl = new QLabel("🎛 Calibration Adjustments", calibGroup);
    iconLbl->setFont(QFont("sans-serif", 11, QFont::Bold));
    headerBox->addWidget(iconLbl);
    headerBox->addStretch();

    auto resetBtn = new QPushButton("Reset to Defaults", calibGroup);
    connect(resetBtn, &QPushButton::clicked, this, &AnalogVUDetailView::resetDefaults);
    headerBox->addWidget(resetBtn);
    calibLayout->addLayout(headerBox);

    auto grid = new QGridLayout();

    // Scale Radius (1.0 .. 1.5)
    grid->addWidget(new QLabel("Scale Radius:", calibGroup), 0, 0);
    m_radiusSlider = new QSlider(Qt::Horizontal, calibGroup);
    m_radiusSlider->setRange(100, 150);
    m_radiusSlider->setValue(static_cast<int>(m_settings.radiusScale * 100));
    m_radiusLbl = new QLabel(QString::number(m_settings.radiusScale, 'f', 2), calibGroup);
    m_radiusLbl->setFixedWidth(40);
    connect(m_radiusSlider, &QSlider::valueChanged, [this](int val) {
        m_settings.radiusScale = val / 100.0;
        m_settings.save();
        m_radiusLbl->setText(QString::number(m_settings.radiusScale, 'f', 2));
        m_vuMeter->setVUSettings(m_settings);
    });
    grid->addWidget(m_radiusSlider, 0, 1);
    grid->addWidget(m_radiusLbl, 0, 2);

    // Pivot Y (1.0 .. 2.0)
    grid->addWidget(new QLabel("Pivot Position (Y):", calibGroup), 0, 3);
    m_pivotYSlider = new QSlider(Qt::Horizontal, calibGroup);
    m_pivotYSlider->setRange(100, 200);
    m_pivotYSlider->setValue(static_cast<int>(m_settings.pivotY * 100));
    m_pivotYLbl = new QLabel(QString::number(m_settings.pivotY, 'f', 2), calibGroup);
    m_pivotYLbl->setFixedWidth(40);
    connect(m_pivotYSlider, &QSlider::valueChanged, [this](int val) {
        m_settings.pivotY = val / 100.0;
        m_settings.save();
        m_pivotYLbl->setText(QString::number(m_settings.pivotY, 'f', 2));
        m_vuMeter->setVUSettings(m_settings);
    });
    grid->addWidget(m_pivotYSlider, 0, 4);
    grid->addWidget(m_pivotYLbl, 0, 5);

    // Needle Extension (0 .. 60)
    grid->addWidget(new QLabel("Needle Extension:", calibGroup), 1, 0);
    m_needleExtSlider = new QSlider(Qt::Horizontal, calibGroup);
    m_needleExtSlider->setRange(0, 60);
    m_needleExtSlider->setValue(static_cast<int>(m_settings.needleExtension));
    m_needleExtLbl = new QLabel(QString::number(m_settings.needleExtension, 'f', 1), calibGroup);
    m_needleExtLbl->setFixedWidth(40);
    connect(m_needleExtSlider, &QSlider::valueChanged, [this](int val) {
        m_settings.needleExtension = val;
        m_settings.save();
        m_needleExtLbl->setText(QString::number(m_settings.needleExtension, 'f', 1));
        m_vuMeter->setVUSettings(m_settings);
    });
    grid->addWidget(m_needleExtSlider, 1, 1);
    grid->addWidget(m_needleExtLbl, 1, 2);

    // Ambient Glow (0.0 .. 1.0)
    grid->addWidget(new QLabel("Ambient Glow:", calibGroup), 1, 3);
    m_ambientGlowSlider = new QSlider(Qt::Horizontal, calibGroup);
    m_ambientGlowSlider->setRange(0, 100);
    m_ambientGlowSlider->setValue(static_cast<int>(m_settings.ambientGlow * 100));
    m_ambientGlowLbl = new QLabel(QString::number(m_settings.ambientGlow, 'f', 2), calibGroup);
    m_ambientGlowLbl->setFixedWidth(40);
    connect(m_ambientGlowSlider, &QSlider::valueChanged, [this](int val) {
        m_settings.ambientGlow = val / 100.0;
        m_settings.save();
        m_ambientGlowLbl->setText(QString::number(m_settings.ambientGlow, 'f', 2));
        m_vuMeter->setVUSettings(m_settings);
    });
    grid->addWidget(m_ambientGlowSlider, 1, 4);
    grid->addWidget(m_ambientGlowLbl, 1, 5);

    // Focused Hot Spot (0.0 .. 1.0)
    grid->addWidget(new QLabel("Focused Hot Spot:", calibGroup), 2, 0);
    m_hotSpotSlider = new QSlider(Qt::Horizontal, calibGroup);
    m_hotSpotSlider->setRange(0, 100);
    m_hotSpotSlider->setValue(static_cast<int>(m_settings.hotSpotAlpha * 100));
    m_hotSpotLbl = new QLabel(QString::number(m_settings.hotSpotAlpha, 'f', 2), calibGroup);
    m_hotSpotLbl->setFixedWidth(40);
    connect(m_hotSpotSlider, &QSlider::valueChanged, [this](int val) {
        m_settings.hotSpotAlpha = val / 100.0;
        m_settings.save();
        m_hotSpotLbl->setText(QString::number(m_settings.hotSpotAlpha, 'f', 2));
        m_vuMeter->setVUSettings(m_settings);
    });
    grid->addWidget(m_hotSpotSlider, 2, 1);
    grid->addWidget(m_hotSpotLbl, 2, 2);

    // Overall Light Wash (0.0 .. 0.4)
    grid->addWidget(new QLabel("Overall Light Wash:", calibGroup), 2, 3);
    m_lightWashSlider = new QSlider(Qt::Horizontal, calibGroup);
    m_lightWashSlider->setRange(0, 40);
    m_lightWashSlider->setValue(static_cast<int>(m_settings.lightWash * 100));
    m_lightWashLbl = new QLabel(QString::number(m_settings.lightWash, 'f', 2), calibGroup);
    m_lightWashLbl->setFixedWidth(40);
    connect(m_lightWashSlider, &QSlider::valueChanged, [this](int val) {
        m_settings.lightWash = val / 100.0;
        m_settings.save();
        m_lightWashLbl->setText(QString::number(m_settings.lightWash, 'f', 2));
        m_vuMeter->setVUSettings(m_settings);
    });
    grid->addWidget(m_lightWashSlider, 2, 4);
    grid->addWidget(m_lightWashLbl, 2, 5);

    calibLayout->addLayout(grid);
    mainLayout->addWidget(calibGroup);
}

void AnalogVUDetailView::resetDefaults() {
    m_settings.reset();
    m_themeCombo->setCurrentIndex(static_cast<int>(m_settings.theme));
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
    m_themeCombo->setCurrentIndex(static_cast<int>(m_settings.theme));
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
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    m_spectrumView = new SpectrumView(m_engine, this);
    mainLayout->addWidget(m_spectrumView, 1);

    auto panelGroup = new QGroupBox("Spectrum Settings Controls", this);
    auto panelLayout = new QVBoxLayout(panelGroup);

    m_channelCombo = new QComboBox(panelGroup);

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
    headerBox->addWidget(new QLabel("🎛 Analyzer Configuration", panelGroup));
    headerBox->addStretch();
    auto resetBtn = new QPushButton("Reset to Defaults", panelGroup);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombo]() {
        m_engine->resetToDefaults();
        m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
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

    rowLayout->addWidget(new QLabel("Source:", panelGroup));
    m_sourceCombo = new QComboBox(panelGroup);
    m_sourceCombo->addItems({"Capture", "Playback"});
    m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, updateChannelCombo](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombo();
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_sourceCombo);

    rowLayout->addWidget(new QLabel("Channel:", panelGroup));
    updateChannelCombo();
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx == 0)
            m_engine->channel.reset();
        else
            m_engine->channel = idx - 1;
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_channelCombo);

    rowLayout->addWidget(new QLabel("Bins:", panelGroup));
    m_binsSpin = new QSpinBox(panelGroup);
    m_binsSpin->setRange(2, 100);
    m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nBins = static_cast<size_t>(val);
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_binsSpin);

    m_rangeLbl = new QLabel(
        QString("Range: %1 - %2 Hz").arg(static_cast<int>(m_engine->minFreq)).arg(static_cast<int>(m_engine->maxFreq)),
        panelGroup);
    rowLayout->addWidget(m_rangeLbl);

    m_rangeSlider = new LogRangeSlider(panelGroup);
    m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);
    connect(m_rangeSlider, &LogRangeSlider::rangeChanged, [this](double minF, double maxF) {
        m_engine->minFreq = minF;
        m_engine->maxFreq = maxF;
        m_rangeLbl->setText(QString("Range: %1 - %2 Hz").arg(static_cast<int>(minF)).arg(static_cast<int>(maxF)));
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_rangeSlider, 1);

    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelGroup);
}

// ==================== SpectrogramDetailView ====================

SpectrogramDetailView::SpectrogramDetailView(std::shared_ptr<SpectrogramEngine> engine,
                                             std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
    setupUi();
}

void SpectrogramDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    m_spectrogramView = new SpectrogramView(m_engine, this);
    mainLayout->addWidget(m_spectrogramView, 1);

    auto panelGroup = new QGroupBox("Spectroscope Settings Controls", this);
    auto panelLayout = new QVBoxLayout(panelGroup);

    m_channelCombo = new QComboBox(panelGroup);

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
    headerBox->addWidget(new QLabel("🎛 Spectrogram Configuration", panelGroup));
    headerBox->addStretch();
    auto resetBtn = new QPushButton("Reset to Defaults", panelGroup);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombo]() {
        m_engine->resetToDefaults();
        m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        updateChannelCombo();
        m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
        m_modeCombo->setCurrentIndex(m_engine->show3D ? 1 : 0);
        m_spectrogramView->update();
    });
    headerBox->addWidget(resetBtn);
    panelLayout->addLayout(headerBox);

    auto rowLayout = new QHBoxLayout();

    rowLayout->addWidget(new QLabel("Source:", panelGroup));
    m_sourceCombo = new QComboBox(panelGroup);
    m_sourceCombo->addItems({"Capture", "Playback"});
    m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, updateChannelCombo](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombo();
        m_spectrogramView->update();
    });
    rowLayout->addWidget(m_sourceCombo);

    rowLayout->addWidget(new QLabel("Channel:", panelGroup));
    updateChannelCombo();
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx == 0)
            m_engine->channel.reset();
        else
            m_engine->channel = idx - 1;
        m_spectrogramView->update();
    });
    rowLayout->addWidget(m_channelCombo);

    rowLayout->addWidget(new QLabel("Bins:", panelGroup));
    m_binsSpin = new QSpinBox(panelGroup);
    m_binsSpin->setRange(20, 500);
    m_binsSpin->setSingleStep(20);
    m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nBins = static_cast<size_t>(val);
        m_spectrogramView->update();
    });
    rowLayout->addWidget(m_binsSpin);

    rowLayout->addWidget(new QLabel("Display Mode:", panelGroup));
    m_modeCombo = new QComboBox(panelGroup);
    m_modeCombo->addItems({"2D Waterfall", "3D Landscape"});
    m_modeCombo->setCurrentIndex(m_engine->show3D ? 1 : 0);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->show3D = (idx == 1);
        m_spectrogramView->update();
    });
    rowLayout->addWidget(m_modeCombo);

    rowLayout->addStretch();
    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelGroup);
}

// ==================== VectorScopeDetailView ====================

VectorScopeDetailView::VectorScopeDetailView(std::shared_ptr<VectorScopeEngine> engine, QWidget* parent)
    : QWidget(parent), m_engine(engine) {
    setupUi();
}

void VectorScopeDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    m_vectorView = new VectorScopeView(m_engine, this);
    mainLayout->addWidget(m_vectorView, 1);

    auto panelGroup = new QGroupBox("Vector Scope Settings Controls", this);
    auto panelLayout = new QVBoxLayout(panelGroup);

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(new QLabel("🎛 Stereo Phase Oscilloscope Configuration", panelGroup));
    headerBox->addStretch();
    auto resetBtn = new QPushButton("Reset to Defaults", panelGroup);
    connect(resetBtn, &QPushButton::clicked, [this]() {
        m_engine->resetToDefaults();
        m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        m_framesSpin->setValue(static_cast<int>(m_engine->nFrames));
        m_modeCombo->setCurrentIndex(m_engine->showParticles ? 1 : 0);
        m_autoScaleCheck->setChecked(m_engine->autoScale);
        m_vectorView->update();
    });
    headerBox->addWidget(resetBtn);
    panelLayout->addLayout(headerBox);

    auto rowLayout = new QHBoxLayout();

    rowLayout->addWidget(new QLabel("Source:", panelGroup));
    m_sourceCombo = new QComboBox(panelGroup);
    m_sourceCombo->addItems({"Capture", "Playback"});
    m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->isCapture = (idx == 0);
        m_vectorView->update();
    });
    rowLayout->addWidget(m_sourceCombo);

    rowLayout->addWidget(new QLabel("Frames:", panelGroup));
    m_framesSpin = new QSpinBox(panelGroup);
    m_framesSpin->setRange(128, 4096);
    m_framesSpin->setSingleStep(128);
    m_framesSpin->setValue(static_cast<int>(m_engine->nFrames));
    connect(m_framesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nFrames = static_cast<size_t>(val);
        m_vectorView->update();
    });
    rowLayout->addWidget(m_framesSpin);

    rowLayout->addWidget(new QLabel("Display Mode:", panelGroup));
    m_modeCombo = new QComboBox(panelGroup);
    m_modeCombo->addItems({"Line", "Particles"});
    m_modeCombo->setCurrentIndex(m_engine->showParticles ? 1 : 0);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->showParticles = (idx == 1);
        m_vectorView->update();
    });
    rowLayout->addWidget(m_modeCombo);

    m_autoScaleCheck = new QCheckBox("Auto Scale", panelGroup);
    m_autoScaleCheck->setChecked(m_engine->autoScale);
    connect(m_autoScaleCheck, &QCheckBox::toggled, [this](bool chk) {
        m_engine->autoScale = chk;
        m_vectorView->update();
    });
    rowLayout->addWidget(m_autoScaleCheck);

    rowLayout->addStretch();
    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelGroup);
}
