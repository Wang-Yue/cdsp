#include "ui/VisualizerDetailViews.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
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

    auto vuHeaderLayout = new QHBoxLayout();
    vuHeaderLayout->setSpacing(8);
    vuHeaderLayout->addStretch();

    auto vuThemeLbl = new QLabel(tr("Theme:"), this);
    m_vuThemeCombo = new QComboBox(this);
    m_vuThemeCombo->addItem("Vintage Amber", static_cast<int>(VUTheme::VintageAmber));
    m_vuThemeCombo->addItem("Dark Stealth", static_cast<int>(VUTheme::DarkStealth));
    m_vuThemeCombo->addItem("Warm Tube", static_cast<int>(VUTheme::WarmTube));
    vuHeaderLayout->addWidget(vuThemeLbl);
    vuHeaderLayout->addWidget(m_vuThemeCombo);
    mainLayout->addLayout(vuHeaderLayout);

    int curThemeIdx = m_vuThemeCombo->findData(static_cast<int>(m_settings.theme));
    if (curThemeIdx >= 0) {
        m_vuThemeCombo->setCurrentIndex(curThemeIdx);
    }

    connect(m_vuThemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (!m_vuMeter)
            return;
        m_settings.theme = static_cast<VUTheme>(m_vuThemeCombo->itemData(idx).toInt());
        m_vuMeter->setVUSettings(m_settings);
        m_settings.save();
    });

    // VU Meter Display
    m_vuMeter = new AnalogVUMeterView(this);
    if (m_monitoring)
        m_vuMeter->setLevelState(&m_monitoring->levelState);
    m_vuMeter->setVUSettings(m_settings);
    mainLayout->addWidget(m_vuMeter, 1);

    // Parameter Controls using standard QGroupBox and QFormLayout
    auto groupsLayout = new QHBoxLayout();

    auto makeSliderRow = [this](QWidget* parent, QSlider*& slider, QLabel*& label, int minVal, int maxVal, int curVal,
                                int precision, auto onValueChanged) -> QWidget* {
        auto widget = new QWidget(parent);
        auto row = new QHBoxLayout(widget);
        row->setContentsMargins(0, 0, 0, 0);

        slider = new QSlider(Qt::Horizontal, widget);
        slider->setRange(minVal, maxVal);
        slider->setValue(curVal);

        label = new QLabel(QString::number(curVal / (precision == 1 ? 1.0 : 100.0), 'f', precision), widget);
        label->setFixedWidth(45);
        label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

        connect(slider, &QSlider::valueChanged, [this, label, precision, onValueChanged](int val) {
            double value = precision == 1 ? val : (val / 100.0);
            label->setText(QString::number(value, 'f', precision));
            onValueChanged(value);
            m_vuMeter->setVUSettings(m_settings);
        });

        row->addWidget(slider);
        row->addWidget(label);
        return widget;
    };

    // Calibration Group
    auto calibGroup = new QGroupBox(tr("Calibration"), this);
    auto calibForm = new QFormLayout(calibGroup);

    calibForm->addRow(tr("Scale Radius:"),
                      makeSliderRow(calibGroup, m_radiusSlider, m_radiusLbl, 100, 150,
                                    static_cast<int>(m_settings.radiusScale * 100), 2, [this](double val) {
                                        m_settings.radiusScale = val;
                                        m_settings.save();
                                    }));

    calibForm->addRow(tr("Pivot Position (Y):"),
                      makeSliderRow(calibGroup, m_pivotYSlider, m_pivotYLbl, 100, 200,
                                    static_cast<int>(m_settings.pivotY * 100), 2, [this](double val) {
                                        m_settings.pivotY = val;
                                        m_settings.save();
                                    }));

    calibForm->addRow(tr("Needle Extension:"),
                      makeSliderRow(calibGroup, m_needleExtSlider, m_needleExtLbl, 0, 60,
                                    static_cast<int>(m_settings.needleExtension), 1, [this](double val) {
                                        m_settings.needleExtension = val;
                                        m_settings.save();
                                    }));

    groupsLayout->addWidget(calibGroup);

    // Lighting Group
    auto lightingGroup = new QGroupBox(tr("Lighting"), this);
    auto lightingForm = new QFormLayout(lightingGroup);

    lightingForm->addRow(tr("Ambient Glow:"),
                         makeSliderRow(lightingGroup, m_ambientGlowSlider, m_ambientGlowLbl, 0, 100,
                                       static_cast<int>(m_settings.ambientGlow * 100), 2, [this](double val) {
                                           m_settings.ambientGlow = val;
                                           m_settings.save();
                                       }));

    lightingForm->addRow(tr("Focused Hot Spot:"),
                         makeSliderRow(lightingGroup, m_hotSpotSlider, m_hotSpotLbl, 0, 100,
                                       static_cast<int>(m_settings.hotSpotAlpha * 100), 2, [this](double val) {
                                           m_settings.hotSpotAlpha = val;
                                           m_settings.save();
                                       }));

    lightingForm->addRow(tr("Overall Light Wash:"),
                         makeSliderRow(lightingGroup, m_lightWashSlider, m_lightWashLbl, 0, 40,
                                       static_cast<int>(m_settings.lightWash * 100), 2, [this](double val) {
                                           m_settings.lightWash = val;
                                           m_settings.save();
                                       }));

    groupsLayout->addWidget(lightingGroup);
    mainLayout->addLayout(groupsLayout);

    // Bottom Action Toolbar
    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto resetBtn = new QPushButton(tr("Reset to Defaults"), this);
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, &AnalogVUDetailView::resetDefaults);
    btnLayout->addWidget(resetBtn);
    mainLayout->addLayout(btnLayout);
}

void AnalogVUDetailView::resetDefaults() {
    m_settings.reset();
    if (m_vuThemeCombo) {
        int themeIdx = m_vuThemeCombo->findData(static_cast<int>(m_settings.theme));
        if (themeIdx >= 0) {
            m_vuThemeCombo->blockSignals(true);
            m_vuThemeCombo->setCurrentIndex(themeIdx);
            m_vuThemeCombo->blockSignals(false);
        }
    }
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
    if (m_vuThemeCombo) {
        int themeIdx = m_vuThemeCombo->findData(static_cast<int>(m_settings.theme));
        if (themeIdx >= 0) {
            m_vuThemeCombo->blockSignals(true);
            m_vuThemeCombo->setCurrentIndex(themeIdx);
            m_vuThemeCombo->blockSignals(false);
        }
    }
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

    // Spectrum Display
    m_spectrumView = new SpectrumView(m_engine, this);
    mainLayout->addWidget(m_spectrumView, 1);

    // Controls using standard QGroupBox and QFormLayout
    auto groupsLayout = new QHBoxLayout();

    // Source & Channel Group
    auto sourceGroup = new QGroupBox(tr("Input Source"), this);
    auto sourceForm = new QFormLayout(sourceGroup);

    m_sourceTabBar = new QTabBar(sourceGroup);
    m_sourceTabBar->addTab(tr("Capture"));
    m_sourceTabBar->addTab(tr("Playback"));
    m_sourceTabBar->setDrawBase(false);
    m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    sourceForm->addRow(tr("Source:"), m_sourceTabBar);

    m_channelCombo = new QComboBox(sourceGroup);
    auto updateChannelCombo = [this]() {
        m_channelCombo->blockSignals(true);
        m_channelCombo->clear();
        m_channelCombo->addItem(tr("Average"));
        int count = 8;
        if (m_devices) {
            int ch = m_engine->isCapture ? m_devices->captureConfig.channels : m_devices->playbackConfig.channels;
            if (ch > 0)
                count = std::max(2, ch);
        }
        for (int i = 0; i < count; ++i) {
            m_channelCombo->addItem(tr("Channel %1").arg(i + 1));
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
    updateChannelCombo();

    connect(m_sourceTabBar, &QTabBar::currentChanged, [this, updateChannelCombo](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombo();
        m_spectrumView->update();
    });

    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx == 0)
            m_engine->channel.reset();
        else
            m_engine->channel = idx - 1;
        m_spectrumView->update();
    });

    sourceForm->addRow(tr("Channel:"), m_channelCombo);
    groupsLayout->addWidget(sourceGroup);

    // Display & Analysis Options Group
    auto displayGroup = new QGroupBox(tr("Display Options"), this);
    auto displayForm = new QFormLayout(displayGroup);

    m_binsSpin = new QSpinBox(displayGroup);
    m_binsSpin->setRange(2, 100);
    m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nBins = static_cast<size_t>(val);
        m_spectrumView->update();
    });
    displayForm->addRow(tr("FFT Bins:"), m_binsSpin);

    auto rangeWidget = new QWidget(displayGroup);
    auto rangeLayout = new QVBoxLayout(rangeWidget);
    rangeLayout->setContentsMargins(0, 0, 0, 0);

    m_rangeSlider = new LogRangeSlider(rangeWidget);
    m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);

    m_rangeLbl = new QLabel(
        tr("Range: %1 - %2 Hz").arg(static_cast<int>(m_engine->minFreq)).arg(static_cast<int>(m_engine->maxFreq)),
        rangeWidget);

    connect(m_rangeSlider, &LogRangeSlider::rangeChanged, [this](double minF, double maxF) {
        m_engine->minFreq = minF;
        m_engine->maxFreq = maxF;
        m_rangeLbl->setText(tr("Range: %1 - %2 Hz").arg(static_cast<int>(minF)).arg(static_cast<int>(maxF)));
        m_spectrumView->update();
    });

    rangeLayout->addWidget(m_rangeSlider);
    rangeLayout->addWidget(m_rangeLbl);
    displayForm->addRow(tr("Frequency Range:"), rangeWidget);

    groupsLayout->addWidget(displayGroup);
    mainLayout->addLayout(groupsLayout);

    // Bottom Action Toolbar
    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto resetBtn = new QPushButton(tr("Reset to Defaults"), this);
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombo]() {
        m_engine->resetToDefaults();
        m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        updateChannelCombo();
        m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
        m_rangeSlider->setRange(m_engine->minFreq, m_engine->maxFreq);
        m_rangeLbl->setText(
            tr("Range: %1 - %2 Hz").arg(static_cast<int>(m_engine->minFreq)).arg(static_cast<int>(m_engine->maxFreq)));
        m_spectrumView->update();
    });
    btnLayout->addWidget(resetBtn);
    mainLayout->addLayout(btnLayout);
}

// ==================== SpectrogramDetailView ====================

SpectrogramDetailView::SpectrogramDetailView(std::shared_ptr<SpectrogramEngine> engine,
                                             std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
    setupUi();
}

void SpectrogramDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);

    // Spectrogram Display
    m_spectrogramView = new SpectrogramView(m_engine, this);
    mainLayout->addWidget(m_spectrogramView, 1);

    // Controls using standard QGroupBox and QFormLayout
    auto groupsLayout = new QHBoxLayout();

    // Source & Channel Group
    auto sourceGroup = new QGroupBox(tr("Input Source"), this);
    auto sourceForm = new QFormLayout(sourceGroup);

    m_sourceTabBar = new QTabBar(sourceGroup);
    m_sourceTabBar->addTab(tr("Capture"));
    m_sourceTabBar->addTab(tr("Playback"));
    m_sourceTabBar->setDrawBase(false);
    m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    sourceForm->addRow(tr("Source:"), m_sourceTabBar);

    m_channelCombo = new QComboBox(sourceGroup);
    auto updateChannelCombo = [this]() {
        m_channelCombo->blockSignals(true);
        m_channelCombo->clear();
        m_channelCombo->addItem(tr("Average"));
        int count = 8;
        if (m_devices) {
            int ch = m_engine->isCapture ? m_devices->captureConfig.channels : m_devices->playbackConfig.channels;
            if (ch > 0)
                count = std::max(2, ch);
        }
        for (int i = 0; i < count; ++i) {
            m_channelCombo->addItem(tr("Channel %1").arg(i + 1));
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
    updateChannelCombo();

    connect(m_sourceTabBar, &QTabBar::currentChanged, [this, updateChannelCombo](int idx) {
        m_engine->isCapture = (idx == 0);
        updateChannelCombo();
        m_spectrogramView->update();
    });

    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx == 0)
            m_engine->channel.reset();
        else
            m_engine->channel = idx - 1;
        m_spectrogramView->update();
    });

    sourceForm->addRow(tr("Channel:"), m_channelCombo);
    groupsLayout->addWidget(sourceGroup);

    // Display & Analysis Options Group
    auto displayGroup = new QGroupBox(tr("Display Options"), this);
    auto displayForm = new QFormLayout(displayGroup);

    m_binsSpin = new QSpinBox(displayGroup);
    m_binsSpin->setRange(20, 500);
    m_binsSpin->setSingleStep(20);
    m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
    connect(m_binsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        m_engine->nBins = static_cast<size_t>(val);
        m_spectrogramView->update();
    });
    displayForm->addRow(tr("FFT Bins:"), m_binsSpin);

    m_modeTabBar = new QTabBar(displayGroup);
    m_modeTabBar->addTab(tr("2D Waterfall"));
    m_modeTabBar->addTab(tr("3D Landscape"));
    m_modeTabBar->setDrawBase(false);
    m_modeTabBar->setCurrentIndex(m_engine->show3D ? 1 : 0);
    connect(m_modeTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_engine->show3D = (idx == 1);
        m_spectrogramView->update();
    });
    displayForm->addRow(tr("Display Mode:"), m_modeTabBar);

    m_paletteCombo = new QComboBox(displayGroup);
    m_paletteCombo->addItem(tr("Classic"), static_cast<int>(ColorPalette::Classic));
    m_paletteCombo->addItem(tr("Viridis"), static_cast<int>(ColorPalette::Viridis));
    m_paletteCombo->addItem(tr("Magma"), static_cast<int>(ColorPalette::Magma));
    m_paletteCombo->addItem(tr("Plasma"), static_cast<int>(ColorPalette::Plasma));
    m_paletteCombo->addItem(tr("Inferno"), static_cast<int>(ColorPalette::Inferno));
    m_paletteCombo->addItem(tr("Jet"), static_cast<int>(ColorPalette::Jet));

    int palIdx = m_paletteCombo->findData(static_cast<int>(m_engine->colorPalette));
    if (palIdx >= 0) {
        m_paletteCombo->setCurrentIndex(palIdx);
    }
    connect(m_paletteCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (idx >= 0) {
            m_engine->colorPalette = static_cast<ColorPalette>(m_paletteCombo->currentData().toInt());
            m_spectrogramView->setHistory(m_engine->history, m_engine->show3D, m_engine->colorPalette);
        }
    });
    displayForm->addRow(tr("Color Palette:"), m_paletteCombo);

    groupsLayout->addWidget(displayGroup);
    mainLayout->addLayout(groupsLayout);

    // Bottom Action Toolbar
    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto resetBtn = new QPushButton(tr("Reset to Defaults"), this);
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, [this, updateChannelCombo]() {
        m_engine->resetToDefaults();
        m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        updateChannelCombo();
        m_binsSpin->setValue(static_cast<int>(m_engine->nBins));
        m_modeTabBar->setCurrentIndex(m_engine->show3D ? 1 : 0);
        int palIdx = m_paletteCombo->findData(static_cast<int>(m_engine->colorPalette));
        if (palIdx >= 0)
            m_paletteCombo->setCurrentIndex(palIdx);
        m_spectrogramView->update();
    });
    btnLayout->addWidget(resetBtn);
    mainLayout->addLayout(btnLayout);
}

// ==================== VectorScopeDetailView ====================

VectorScopeDetailView::VectorScopeDetailView(std::shared_ptr<VectorScopeEngine> engine,
                                             std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_devices(devices) {
    setupUi();
}

void VectorScopeDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);

    // VectorScope Display
    m_vectorView = new VectorScopeView(m_engine, this);
    mainLayout->addWidget(m_vectorView, 1);

    // Controls using standard QGroupBox and QFormLayout
    auto groupsLayout = new QHBoxLayout();

    // Source Group
    auto sourceGroup = new QGroupBox(tr("Input Source"), this);
    auto sourceForm = new QFormLayout(sourceGroup);

    m_sourceTabBar = new QTabBar(sourceGroup);
    m_sourceTabBar->addTab(tr("Capture"));
    m_sourceTabBar->addTab(tr("Playback"));
    m_sourceTabBar->setDrawBase(false);
    m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
    connect(m_sourceTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_engine->isCapture = (idx == 0);
        m_vectorView->update();
    });
    sourceForm->addRow(tr("Source:"), m_sourceTabBar);

    m_windowCombo = new QComboBox(sourceGroup);
    m_windowCombo->addItem(tr("Fast / Snappy (Δt = 25 ms)"), static_cast<int>(VectorScopeWindow::Fast));
    m_windowCombo->addItem(tr("Smooth Persistence (Δt = 50 ms)"), static_cast<int>(VectorScopeWindow::Smooth));
    m_windowCombo->addItem(tr("Long Glow / Trace (Δt = 100 ms)"), static_cast<int>(VectorScopeWindow::Long));
    m_windowCombo->setCurrentIndex(static_cast<int>(m_engine->window));
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_engine->window = static_cast<VectorScopeWindow>(idx);
        m_vectorView->update();
    });
    sourceForm->addRow(tr("Window / Duration:"), m_windowCombo);

    groupsLayout->addWidget(sourceGroup);

    // Display Options Group
    auto displayGroup = new QGroupBox(tr("Display Options"), this);
    auto displayForm = new QFormLayout(displayGroup);

    m_modeTabBar = new QTabBar(displayGroup);
    m_modeTabBar->addTab(tr("Line"));
    m_modeTabBar->addTab(tr("Particles"));
    m_modeTabBar->setDrawBase(false);
    m_modeTabBar->setCurrentIndex(m_engine->showParticles ? 1 : 0);
    connect(m_modeTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_engine->showParticles = (idx == 1);
        m_vectorView->update();
    });
    displayForm->addRow(tr("Display Mode:"), m_modeTabBar);

    m_autoScaleCheck = new QCheckBox(tr("Auto Scale"), displayGroup);
    m_autoScaleCheck->setChecked(m_engine->autoScale);
    connect(m_autoScaleCheck, &QCheckBox::toggled, [this](bool chk) {
        m_engine->autoScale = chk;
        m_vectorView->update();
    });
    displayForm->addRow(tr("Scaling:"), m_autoScaleCheck);

    groupsLayout->addWidget(displayGroup);
    mainLayout->addLayout(groupsLayout);

    // Bottom Action Toolbar
    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto resetBtn = new QPushButton(tr("Reset to Defaults"), this);
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, [this]() {
        m_engine->resetToDefaults();
        m_sourceTabBar->setCurrentIndex(m_engine->isCapture ? 0 : 1);
        m_windowCombo->setCurrentIndex(static_cast<int>(m_engine->window));
        m_modeTabBar->setCurrentIndex(m_engine->showParticles ? 1 : 0);
        m_autoScaleCheck->setChecked(m_engine->autoScale);
        m_vectorView->update();
    });
    btnLayout->addWidget(resetBtn);
    mainLayout->addLayout(btnLayout);
}
