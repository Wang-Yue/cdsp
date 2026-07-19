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
        m_windowCombo->setCurrentIndex(static_cast<int>(m_engine->windowFunction));
        m_smoothingCombo->setCurrentIndex(static_cast<int>(m_engine->smoothing));
        m_decayCombo->setCurrentIndex(0);
        m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);
        m_rangeLbl->setText(QString("Freq Range: %1 - %2 Hz")
                                .arg(static_cast<int>(m_engine->minFreq))
                                .arg(static_cast<int>(m_engine->maxFreq)));
        m_dbRangeSlider->setRange(m_engine->minDB, m_engine->maxDB);
        m_dbRangeLbl->setText(QString("dB Range: %1 - %2 dB")
                                  .arg(static_cast<int>(m_engine->minDB))
                                  .arg(static_cast<int>(m_engine->maxDB)));
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

    rowLayout->addWidget(new QLabel("FFT Window:", panelGroup));
    m_windowCombo = new QComboBox(panelGroup);
    m_windowCombo->addItems({"Hann", "Hamming", "Blackman", "Flat Top", "Rectangular"});
    m_windowCombo->setCurrentIndex(static_cast<int>(m_engine->windowFunction));
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->windowFunction = static_cast<FFTWindowFunction>(idx);
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_windowCombo);

    rowLayout->addWidget(new QLabel("Smoothing:", panelGroup));
    m_smoothingCombo = new QComboBox(panelGroup);
    m_smoothingCombo->addItems({"Off", "1/3 Octave", "1/6 Octave", "1/12 Octave", "1/24 Octave"});
    m_smoothingCombo->setCurrentIndex(static_cast<int>(m_engine->smoothing));
    connect(m_smoothingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->smoothing = static_cast<OctaveSmoothing>(idx);
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_smoothingCombo);

    rowLayout->addWidget(new QLabel("Peak Decay:", panelGroup));
    m_decayCombo = new QComboBox(panelGroup);
    m_decayCombo->addItems({"Fast (0.95)", "Medium (0.90)", "Slow (0.80)", "Off"});
    connect(m_decayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        static const float decayRates[] = {0.95f, 0.90f, 0.80f, 0.00f};
        m_engine->peakHoldDecayRate = decayRates[idx];
        m_spectrumView->update();
    });
    rowLayout->addWidget(m_decayCombo);

    auto rowLayout2 = new QHBoxLayout();

    m_rangeLbl = new QLabel(QString("Freq Range: %1 - %2 Hz")
                                .arg(static_cast<int>(m_engine->minFreq))
                                .arg(static_cast<int>(m_engine->maxFreq)),
                            panelGroup);
    rowLayout2->addWidget(m_rangeLbl);

    m_rangeSlider = new LogRangeSlider(panelGroup);
    m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);
    connect(m_rangeSlider, &LogRangeSlider::rangeChanged, [this](double minF, double maxF) {
        m_engine->minFreq = minF;
        m_engine->maxFreq = maxF;
        m_rangeLbl->setText(QString("Freq Range: %1 - %2 Hz").arg(static_cast<int>(minF)).arg(static_cast<int>(maxF)));
        m_spectrumView->update();
    });
    rowLayout2->addWidget(m_rangeSlider, 1);

    m_dbRangeLbl = new QLabel(
        QString("dB Range: %1 - %2 dB").arg(static_cast<int>(m_engine->minDB)).arg(static_cast<int>(m_engine->maxDB)),
        panelGroup);
    rowLayout2->addWidget(m_dbRangeLbl);

    m_dbRangeSlider = new LogRangeSlider(panelGroup);
    m_dbRangeSlider->setLogarithmic(false);
    m_dbRangeSlider->setMinMaxBounds(-120.0, 0.0);
    m_dbRangeSlider->setRange(m_engine->minDB, m_engine->maxDB);
    connect(m_dbRangeSlider, &LogRangeSlider::rangeChanged, [this](double minDb, double maxDb) {
        m_engine->minDB = minDb;
        m_engine->maxDB = maxDb;
        m_dbRangeLbl->setText(
            QString("dB Range: %1 - %2 dB").arg(static_cast<int>(minDb)).arg(static_cast<int>(maxDb)));
        m_spectrumView->update();
    });
    rowLayout2->addWidget(m_dbRangeSlider, 1);

    panelLayout->addLayout(rowLayout);
    panelLayout->addLayout(rowLayout2);
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
        m_paletteCombo->setCurrentIndex(static_cast<int>(m_engine->colorPalette));
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

    rowLayout->addWidget(new QLabel("Color Map:", panelGroup));
    m_paletteCombo = new QComboBox(panelGroup);
    m_paletteCombo->addItems({"Classic", "Magma", "Inferno", "Viridis", "Plasma"});
    m_paletteCombo->setCurrentIndex(static_cast<int>(m_engine->colorPalette));
    connect(m_paletteCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->colorPalette = static_cast<ColorPalette>(idx);
        m_spectrogramView->update();
    });
    rowLayout->addWidget(m_paletteCombo);

    rowLayout->addStretch();
    panelLayout->addLayout(rowLayout);
    mainLayout->addWidget(panelGroup);
}

// ==================== VectorScopeDetailView ====================

VectorScopeDetailView::VectorScopeDetailView(std::shared_ptr<VectorScopeEngine> engine,
                                             std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
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

    m_channelLCombo = new QComboBox(panelGroup);
    m_channelRCombo = new QComboBox(panelGroup);

    auto updateChannelCombos = [this]() {
        m_channelLCombo->blockSignals(true);
        m_channelRCombo->blockSignals(true);
        m_channelLCombo->clear();
        m_channelRCombo->clear();
        int count = 8;
        if (m_devices) {
            int ch = m_engine->isCapture ? m_devices->captureConfig.channels : m_devices->playbackConfig.channels;
            if (ch > 0)
                count = std::max(2, ch);
        }
        for (int i = 0; i < count; ++i) {
            m_channelLCombo->addItem(QString("Ch %1 (L)").arg(i + 1));
            m_channelRCombo->addItem(QString("Ch %1 (R)").arg(i + 1));
        }
        m_channelLCombo->setCurrentIndex(std::min(m_engine->channelL, m_channelLCombo->count() - 1));
        m_channelRCombo->setCurrentIndex(std::min(m_engine->channelR, m_channelRCombo->count() - 1));
        m_channelLCombo->blockSignals(false);
        m_channelRCombo->blockSignals(false);
    };

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(new QLabel("🎛 Stereo Phase Oscilloscope Configuration", panelGroup));
    headerBox->addStretch();
    auto resetBtn = new QPushButton("Reset to Defaults", panelGroup);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombos]() {
        m_engine->resetToDefaults();
        m_sourceCombo->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        updateChannelCombos();
        m_framesSpin->setValue(static_cast<int>(m_engine->nFrames));
        m_modeCombo->setCurrentIndex(m_engine->showParticles ? 1 : 0);
        m_decayCombo->setCurrentIndex(1);
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
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, updateChannelCombos](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombos();
        m_vectorView->update();
    });
    rowLayout->addWidget(m_sourceCombo);

    rowLayout->addWidget(new QLabel("Channel L:", panelGroup));
    rowLayout->addWidget(m_channelLCombo);
    connect(m_channelLCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->channelL = idx;
        m_vectorView->update();
    });

    rowLayout->addWidget(new QLabel("Channel R:", panelGroup));
    rowLayout->addWidget(m_channelRCombo);
    connect(m_channelRCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->channelR = idx;
        m_vectorView->update();
    });
    updateChannelCombos();

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

    rowLayout->addWidget(new QLabel("Trace Decay:", panelGroup));
    m_decayCombo = new QComboBox(panelGroup);
    m_decayCombo->addItems({"Fast (0.70)", "Medium (0.85)", "Slow (0.95)", "Infinite (0.99)"});
    m_decayCombo->setCurrentIndex(1);
    connect(m_decayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        static const float decayRates[] = {0.70f, 0.85f, 0.95f, 0.99f};
        m_engine->traceDecayRate = decayRates[idx];
        m_vectorView->update();
    });
    rowLayout->addWidget(m_decayCombo);

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
