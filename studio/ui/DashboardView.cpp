#include "ui/DashboardView.h"

#include "models/PipelineStage.h"
#include "ui/StyleTheme.h"

#include <QComboBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

DashboardView::DashboardView(std::shared_ptr<MonitoringController> monitoring,
                             std::shared_ptr<DSPEngineController> dspController,
                             std::shared_ptr<SpectrumEngine> spectrumEngine,
                             std::shared_ptr<SpectrogramEngine> spectrogramEngine,
                             std::shared_ptr<VectorScopeEngine> vectorScopeEngine, QWidget* parent)
    : QWidget(parent), m_monitoring(monitoring), m_dspController(dspController), m_spectrumEngine(spectrumEngine),
      m_spectrogramEngine(spectrogramEngine), m_vectorScopeEngine(vectorScopeEngine) {
    setupUi();

    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, &DashboardView::refreshMeters);

    if (m_dspController) {
        if (m_dspController->settings()) {
            connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this,
                    &DashboardView::updateVisibility);
            connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this,
                    &DashboardView::updateFaderUi);
            connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this,
                    &DashboardView::updateSignalChain);
        }
        if (m_dspController->pipelineStore()) {
            connect(m_dspController->pipelineStore().get(), &PipelineStore::pipelineChanged, this,
                    &DashboardView::updateSignalChain);
        }
        connect(m_dspController.get(), &DSPEngineController::statusChanged, this, &DashboardView::updateSignalChain);
    }
    updateVisibility();
    updateFaderUi();
    updateSignalChain();
}

void DashboardView::updateVisibility() {
    if (!m_dspController || !m_dspController->settings())
        return;
    auto s = m_dspController->settings();
    if (m_pipelineOverviewWidget)
        m_pipelineOverviewWidget->setVisible(s->showSignalGraphInDashboard);
    if (m_signalGraphCard)
        m_signalGraphCard->setVisible(s->showSignalGraphInDashboard);
    if (m_levelMetersGroup)
        m_levelMetersGroup->setVisible(s->showLevelMetersInDashboard);
    if (m_analogVUGroup)
        m_analogVUGroup->setVisible(s->showAnalogVUInDashboard);
    if (m_spectrumGroup)
        m_spectrumGroup->setVisible(s->showSpectrumInDashboard);
    if (m_spectrogramGroup)
        m_spectrogramGroup->setVisible(s->showSpectrogramInDashboard);
    if (m_vectorScopeGroup)
        m_vectorScopeGroup->setVisible(s->showVectorScopeInDashboard);
}

void DashboardView::updateFaderUi() {
    if (!m_dspController || !m_dspController->settings())
        return;
    auto s = m_dspController->settings();

    for (auto& row : m_faderRows) {
        float vol = s->getVolume(row.fader);
        bool muted = s->getMuted(row.fader);

        row.slider->blockSignals(true);
        row.slider->setValue(static_cast<int>(std::round(vol * 2.0f)));
        row.slider->blockSignals(false);

        row.gainValueLabel->setText(QString::asprintf("%+.1f dB", vol));
        if (vol > 0.0f) {
            row.gainValueLabel->setStyleSheet(
                "font-family: monospace; font-weight: bold; color: #ff3b30; min-width: 75px;");
        } else {
            row.gainValueLabel->setStyleSheet(
                QString("font-family: monospace; font-weight: bold; color: %1; min-width: 75px;")
                    .arg(StyleTheme::textPrimary().name()));
        }

        row.muteBtn->blockSignals(true);
        row.muteBtn->setChecked(muted);
        row.muteBtn->blockSignals(false);

        if (muted) {
            row.muteBtn->setText("🔇");
            row.muteBtn->setStyleSheet(
                "background-color: transparent; color: #ff3b30; border: none; font-size: 16px; font-weight: bold;");
        } else {
            row.muteBtn->setText("🔊");
            row.muteBtn->setStyleSheet("background-color: transparent; color: #8e8e93; border: none; font-size: 16px;");
        }
    }
}

void DashboardView::updateSignalChain() {
    if (m_pipelineOverviewWidget)
        m_pipelineOverviewWidget->rebuildOverview();
    if (m_signalGraphCard)
        m_signalGraphCard->updateCard();
}

void DashboardView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setSpacing(16);

    // 1. Signal Chain Overview Card
    m_pipelineOverviewWidget = new PipelineOverviewWidget(m_dspController, container);
    mainLayout->addWidget(m_pipelineOverviewWidget);

    // 2. Detailed DSP Signal Graph Card
    m_signalGraphCard = new DSPDetailedSignalGraphCard(m_dspController, container);
    mainLayout->addWidget(m_signalGraphCard);

    // 2. Level Meters Card
    m_levelMetersGroup = new QGroupBox("Level Meters", container);
    auto levelLayout = new QHBoxLayout(m_levelMetersGroup);
    m_captureMeters = new LevelMeterView(m_levelMetersGroup);
    m_captureMeters->setLevelState(&m_monitoring->levelState);
    m_playbackMeters = new LevelMeterView(m_levelMetersGroup);
    m_playbackMeters->setLevelState(&m_monitoring->levelState);
    levelLayout->addWidget(m_captureMeters);
    levelLayout->addWidget(m_playbackMeters);
    mainLayout->addWidget(m_levelMetersGroup);

    // 3. Volume Faders Card (All 5 fader rows simultaneously)
    auto faderGroup = new QGroupBox("Volume Faders", container);
    auto faderVLayout = new QVBoxLayout(faderGroup);
    faderVLayout->setSpacing(12);

    struct FaderInfo {
        Fader fader;
        QString name;
    };
    std::vector<FaderInfo> faders = {{Fader::Main, "Main"},
                                     {Fader::Aux1, "Aux 1"},
                                     {Fader::Aux2, "Aux 2"},
                                     {Fader::Aux3, "Aux 3"},
                                     {Fader::Aux4, "Aux 4"}};

    m_faderRows.clear();
    for (const auto& info : faders) {
        auto rowLayout = new QHBoxLayout();

        auto nameLbl = new QLabel(info.name, faderGroup);
        nameLbl->setFixedWidth(100);
        nameLbl->setFont(QFont("System", 13, QFont::DemiBold));

        auto muteBtn = new QPushButton("🔊", faderGroup);
        muteBtn->setCheckable(true);
        muteBtn->setFixedSize(28, 28);
        muteBtn->setFlat(true);
        muteBtn->setCursor(Qt::PointingHandCursor);

        auto slider = new QSlider(Qt::Horizontal, faderGroup);
        slider->setRange(-120, 40); // -60.0 dB to +20.0 dB

        auto gainLbl = new QLabel(" 0.0 dB", faderGroup);
        gainLbl->setFont(QFont("monospace", 11, QFont::Bold));
        gainLbl->setMinimumWidth(90);
        gainLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        Fader f = info.fader;
        connect(muteBtn, &QPushButton::clicked, [this, f]() {
            bool currentMute = m_dspController->settings()->getMuted(f);
            m_dspController->setFaderMute(f, !currentMute);
            updateFaderUi();
        });

        connect(slider, &QSlider::valueChanged, [this, f](int val) {
            float db = val / 2.0f;
            m_dspController->setFaderVolume(f, db, true);
            updateFaderUi();
        });

        rowLayout->addWidget(nameLbl);
        rowLayout->addWidget(muteBtn);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(gainLbl);

        faderVLayout->addLayout(rowLayout);

        m_faderRows.push_back({f, nameLbl, muteBtn, slider, gainLbl});
    }
    mainLayout->addWidget(faderGroup);

    // 4. Analog VU Card
    m_analogVUGroup = new QGroupBox("Analog VU", container);
    auto vuLayout = new QVBoxLayout(m_analogVUGroup);

    auto vuHeaderBox = new QHBoxLayout();
    vuHeaderBox->addStretch();
    auto themeCombo = new QComboBox(m_analogVUGroup);
    themeCombo->addItem("Vintage Amber", static_cast<int>(VUTheme::VintageAmber));
    themeCombo->addItem("Dark Stealth", static_cast<int>(VUTheme::DarkStealth));
    themeCombo->addItem("Warm Tube", static_cast<int>(VUTheme::WarmTube));
    connect(themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, themeCombo](int idx) {
        auto settings = m_analogVUView->vuSettings();
        settings.theme = static_cast<VUTheme>(themeCombo->itemData(idx).toInt());
        m_analogVUView->setVUSettings(settings);
    });
    vuHeaderBox->addWidget(themeCombo);
    vuLayout->addLayout(vuHeaderBox);

    m_analogVUView = new AnalogVUMeterView(m_analogVUGroup);
    m_analogVUView->setLevelState(&m_monitoring->levelState);
    m_analogVUView->setFixedHeight(220);
    vuLayout->addWidget(m_analogVUView);
    mainLayout->addWidget(m_analogVUGroup);

    // 5. Spectrum Card
    m_spectrumGroup = new QGroupBox("Spectrum", container);
    auto specLayout = new QVBoxLayout(m_spectrumGroup);
    m_spectrumView = new SpectrumView(m_spectrumEngine, m_spectrumGroup);
    m_spectrumView->setFixedHeight(160);
    specLayout->addWidget(m_spectrumView);
    mainLayout->addWidget(m_spectrumGroup);

    // 6. Spectrogram Card
    m_spectrogramGroup = new QGroupBox("Spectroscope", container);
    auto spectroLayout = new QVBoxLayout(m_spectrogramGroup);
    m_spectrogramView = new SpectrogramView(m_spectrogramEngine, m_spectrogramGroup);
    m_spectrogramView->setFixedHeight(480);
    spectroLayout->addWidget(m_spectrogramView);
    mainLayout->addWidget(m_spectrogramGroup);

    // 7. Vector Scope Card
    m_vectorScopeGroup = new QGroupBox("Vector Scope", container);
    auto vecLayout = new QVBoxLayout(m_vectorScopeGroup);
    m_vectorScopeView = new VectorScopeView(m_vectorScopeEngine, m_vectorScopeGroup);
    m_vectorScopeView->setFixedHeight(700);
    vecLayout->addWidget(m_vectorScopeView);
    mainLayout->addWidget(m_vectorScopeGroup);

    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void DashboardView::refreshMeters() {
    const auto& st = m_monitoring->levelState;
    if (m_captureMeters)
        m_captureMeters->setLevels(st.captureRms, st.capturePeak, "Capture Levels");
    if (m_playbackMeters)
        m_playbackMeters->setLevels(st.playbackRms, st.playbackPeak, "Playback Levels");

    float leftDB = !st.playbackRms.empty() ? st.playbackRms[0] : -60.0f;
    float rightDB = st.playbackRms.size() > 1 ? st.playbackRms[1] : leftDB;
    if (m_analogVUView)
        m_analogVUView->setLevelDB(leftDB, rightDB);

    if (m_spectrumEngine && m_spectrumView)
        m_spectrumView->setSpectrum(m_spectrumEngine->data);
    if (m_spectrogramEngine && m_spectrogramView)
        m_spectrogramView->setHistory(m_spectrogramEngine->history, m_spectrogramEngine->show3D);
    if (m_vectorScopeEngine && m_vectorScopeView)
        m_vectorScopeView->setSamples(m_vectorScopeEngine->samples, m_vectorScopeEngine->showParticles);
}
