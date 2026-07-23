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
    connect(m_monitoring.get(), &MonitoringController::dashboardVisibilityChanged, this,
            &DashboardView::updateVisibility);

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
    bool showSignalGraph = true;
    bool showLevelMeters = true;
    bool showAnalogVU = true;
    bool showSpectrum = true;
    bool showSpectrogram = true;
    bool showVectorScope = true;

    if (m_dspController && m_dspController->settings()) {
        auto s = m_dspController->settings();
        showSignalGraph = s->showSignalGraphInDashboard;
        showLevelMeters = s->showLevelMetersInDashboard;
        showAnalogVU = s->showAnalogVUInDashboard;
        showSpectrum = s->showSpectrumInDashboard;
        showSpectrogram = s->showSpectrogramInDashboard;
        showVectorScope = s->showVectorScopeInDashboard;
    } else if (m_monitoring) {
        showSignalGraph = m_monitoring->showSignalGraphInDashboard();
        showLevelMeters = m_monitoring->showLevelMetersInDashboard();
        showAnalogVU = m_monitoring->showAnalogVUInDashboard();
        showSpectrum = m_monitoring->showSpectrumInDashboard();
        showSpectrogram = m_monitoring->showSpectrogramInDashboard();
        showVectorScope = m_monitoring->showVectorScopeInDashboard();
    }

    if (m_pipelineOverviewWidget)
        m_pipelineOverviewWidget->setVisible(showSignalGraph);
    if (m_signalGraphCard)
        m_signalGraphCard->setVisible(showSignalGraph);
    if (m_levelMetersGroup)
        m_levelMetersGroup->setVisible(showLevelMeters);
    if (m_analogVUGroup)
        m_analogVUGroup->setVisible(showAnalogVU);
    if (m_spectrumGroup)
        m_spectrumGroup->setVisible(showSpectrum);
    if (m_spectrogramGroup)
        m_spectrogramGroup->setVisible(showSpectrogram);
    if (m_vectorScopeGroup)
        m_vectorScopeGroup->setVisible(showVectorScope);
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
                "font-family: monospace; font-weight: bold; color: #ff3b30; min-width: 70px; max-width: 70px;");
        } else {
            row.gainValueLabel->setStyleSheet(
                QString("font-family: monospace; font-weight: bold; color: %1; min-width: 70px; max-width: 70px;")
                    .arg(StyleTheme::textPrimary().name()));
        }

        row.muteBtn->blockSignals(true);
        row.muteBtn->setChecked(muted);
        row.muteBtn->blockSignals(false);

        if (muted) {
            row.muteBtn->setText("🔇");
            row.muteBtn->setStyleSheet("background-color: transparent; color: #ff3b30; border: none; font-size: 16px; "
                                       "font-weight: bold; padding: 0px; margin: 0px;");
        } else {
            row.muteBtn->setText("🔊");
            row.muteBtn->setStyleSheet(QString("background-color: transparent; color: %1; border: none; font-size: "
                                               "16px; padding: 0px; margin: 0px;")
                                           .arg(StyleTheme::textPrimary().name()));
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
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(20);

    QString cardStyle =
        QString("QGroupBox { "
                "  background-color: %1; "
                "  border: 1px solid %2; "
                "  border-radius: 12px; "
                "  margin-top: 0px; "
                "  padding: 14px; "
                "} "
                "QGroupBox::title { "
                "  subcontrol-origin: margin; "
                "  subcontrol-position: top left; "
                "  padding: 0 4px; "
                "  color: %3; "
                "  font-weight: bold; "
                "}")
            .arg(StyleTheme::cardBg().name(), StyleTheme::border().name(), StyleTheme::textPrimary().name());

    // 1. Signal Chain Overview Card
    m_pipelineOverviewWidget = new PipelineOverviewWidget(m_dspController, container);
    m_pipelineOverviewWidget->setStyleSheet(cardStyle);
    mainLayout->addWidget(m_pipelineOverviewWidget);

    // 2. Detailed DSP Signal Graph Card
    m_signalGraphCard = new DSPDetailedSignalGraphCard(m_dspController, container);
    m_signalGraphCard->setStyleSheet(cardStyle);
    mainLayout->addWidget(m_signalGraphCard);

    // 3. Level Meters Card (Title: "Levels" with "RMS / Peak" on right, "Capture" & "Playback" subheaders)
    m_levelMetersGroup = new QGroupBox(container);
    m_levelMetersGroup->setStyleSheet(cardStyle);
    auto levelCardLayout = new QVBoxLayout(m_levelMetersGroup);
    levelCardLayout->setSpacing(12);

    auto levelHeader = new QHBoxLayout();
    auto levelTitle = new QLabel("Levels", m_levelMetersGroup);
    levelTitle->setFont(QFont("", 13, QFont::Bold));
    auto levelSub = new QLabel("RMS / Peak", m_levelMetersGroup);
    levelSub->setFont(QFont("", 11, QFont::Normal));
    levelSub->setStyleSheet(QString("color: %1;").arg(StyleTheme::textSecondary().name()));
    levelHeader->addWidget(levelTitle);
    levelHeader->addStretch();
    levelHeader->addWidget(levelSub);
    levelCardLayout->addLayout(levelHeader);

    auto levelColumnsLayout = new QHBoxLayout();
    levelColumnsLayout->setSpacing(24);

    // Capture column
    auto capCol = new QVBoxLayout();
    capCol->setSpacing(8);
    auto capLbl = new QLabel("Capture", m_levelMetersGroup);
    capLbl->setFont(QFont("", 12, QFont::Medium));
    capLbl->setStyleSheet(QString("color: %1;").arg(StyleTheme::textSecondary().name()));
    m_captureMeters = new LevelMeterView(m_levelMetersGroup);
    m_captureMeters->setLevelState(&m_monitoring->levelState);
    m_captureMeters->setIsCapture(true);
    capCol->addWidget(capLbl);
    capCol->addWidget(m_captureMeters);
    capCol->addStretch();

    // Playback column
    auto pbCol = new QVBoxLayout();
    pbCol->setSpacing(8);
    auto pbLbl = new QLabel("Playback", m_levelMetersGroup);
    pbLbl->setFont(QFont("", 12, QFont::Medium));
    pbLbl->setStyleSheet(QString("color: %1;").arg(StyleTheme::textSecondary().name()));
    m_playbackMeters = new LevelMeterView(m_levelMetersGroup);
    m_playbackMeters->setLevelState(&m_monitoring->levelState);
    m_playbackMeters->setIsCapture(false);
    pbCol->addWidget(pbLbl);
    pbCol->addWidget(m_playbackMeters);
    pbCol->addStretch();

    levelColumnsLayout->addLayout(capCol);
    levelColumnsLayout->addLayout(pbCol);
    levelCardLayout->addLayout(levelColumnsLayout);
    mainLayout->addWidget(m_levelMetersGroup);

    // 4. Volume Faders Card (All 5 fader rows simultaneously)
    m_faderGroup = new QGroupBox(container);
    m_faderGroup->setStyleSheet(cardStyle);
    auto faderVLayout = new QVBoxLayout(m_faderGroup);
    faderVLayout->setSpacing(16);

    auto faderTitle = new QLabel("Volume Faders", m_faderGroup);
    faderTitle->setFont(QFont("", 13, QFont::Bold));
    faderVLayout->addWidget(faderTitle);

    auto faderRowsVLayout = new QVBoxLayout();
    faderRowsVLayout->setSpacing(12);

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
        rowLayout->setSpacing(12);

        auto nameLbl = new QLabel(info.name, m_faderGroup);
        nameLbl->setFixedWidth(80);
        nameLbl->setFont(QFont("", 12, QFont::Medium));

        auto muteBtn = new QPushButton("🔊", m_faderGroup);
        muteBtn->setCheckable(true);
        muteBtn->setFixedSize(24, 28);
        muteBtn->setFlat(true);
        muteBtn->setCursor(Qt::PointingHandCursor);

        auto slider = new QSlider(Qt::Horizontal, m_faderGroup);
        slider->setRange(-120, 40); // -60.0 dB to +20.0 dB

        auto gainLbl = new QLabel(" 0.0 dB", m_faderGroup);
        gainLbl->setFont(QFont("monospace", 12, QFont::Normal));
        gainLbl->setFixedWidth(70);
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

        faderRowsVLayout->addLayout(rowLayout);

        m_faderRows.push_back({f, nameLbl, muteBtn, slider, gainLbl});
    }
    faderVLayout->addLayout(faderRowsVLayout);
    mainLayout->addWidget(m_faderGroup);

    // 5. Analog VU Card
    m_analogVUGroup = new QGroupBox(container);
    m_analogVUGroup->setStyleSheet(cardStyle);
    auto vuLayout = new QVBoxLayout(m_analogVUGroup);
    vuLayout->setSpacing(12);

    auto vuHeaderBox = new QHBoxLayout();
    auto vuTitle = new QLabel("Analog VU", m_analogVUGroup);
    vuTitle->setFont(QFont("", 13, QFont::Bold));
    vuHeaderBox->addWidget(vuTitle);
    vuHeaderBox->addStretch();
    m_vuThemeCombo = new QComboBox(m_analogVUGroup);
    m_vuThemeCombo->addItem("Vintage Amber", static_cast<int>(VUTheme::VintageAmber));
    m_vuThemeCombo->addItem("Dark Stealth", static_cast<int>(VUTheme::DarkStealth));
    m_vuThemeCombo->addItem("Warm Tube", static_cast<int>(VUTheme::WarmTube));
    vuHeaderBox->addWidget(m_vuThemeCombo);
    vuLayout->addLayout(vuHeaderBox);

    m_analogVUView = new AnalogVUMeterView(m_analogVUGroup);
    m_analogVUView->setLevelState(&m_monitoring->levelState);
    m_analogVUView->setFixedHeight(220);
    vuLayout->addWidget(m_analogVUView);

    int curThemeIdx = m_vuThemeCombo->findData(static_cast<int>(m_analogVUView->vuSettings().theme));
    if (curThemeIdx >= 0) {
        m_vuThemeCombo->setCurrentIndex(curThemeIdx);
    }

    connect(m_vuThemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (!m_analogVUView)
            return;
        auto settings = m_analogVUView->vuSettings();
        settings.theme = static_cast<VUTheme>(m_vuThemeCombo->itemData(idx).toInt());
        m_analogVUView->setVUSettings(settings);
        settings.save();
    });

    mainLayout->addWidget(m_analogVUGroup);

    // 6. Spectrum Card
    m_spectrumGroup = new QGroupBox(container);
    m_spectrumGroup->setStyleSheet(cardStyle);
    auto specLayout = new QVBoxLayout(m_spectrumGroup);
    specLayout->setSpacing(12);
    auto specTitle = new QLabel("Spectrum", m_spectrumGroup);
    specTitle->setFont(QFont("", 13, QFont::Bold));
    specLayout->addWidget(specTitle);
    m_spectrumView = new SpectrumView(m_spectrumEngine, m_spectrumGroup);
    m_spectrumView->setFixedHeight(160);
    specLayout->addWidget(m_spectrumView);
    mainLayout->addWidget(m_spectrumGroup);

    // 7. Spectrogram Card
    m_spectrogramGroup = new QGroupBox(container);
    m_spectrogramGroup->setStyleSheet(cardStyle);
    auto spectroLayout = new QVBoxLayout(m_spectrogramGroup);
    spectroLayout->setSpacing(12);
    auto spectroTitle = new QLabel("Spectroscope", m_spectrogramGroup);
    spectroTitle->setFont(QFont("", 13, QFont::Bold));
    spectroLayout->addWidget(spectroTitle);
    m_spectrogramView = new SpectrogramView(m_spectrogramEngine, m_spectrogramGroup);
    m_spectrogramView->setFixedHeight(480);
    spectroLayout->addWidget(m_spectrogramView);
    mainLayout->addWidget(m_spectrogramGroup);

    // 8. Vector Scope Card
    m_vectorScopeGroup = new QGroupBox(container);
    m_vectorScopeGroup->setStyleSheet(cardStyle);
    auto vecLayout = new QVBoxLayout(m_vectorScopeGroup);
    vecLayout->setSpacing(12);
    auto vecTitle = new QLabel("Vector Scope", m_vectorScopeGroup);
    vecTitle->setFont(QFont("", 13, QFont::Bold));
    vecLayout->addWidget(vecTitle);
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
        m_captureMeters->setLevels(st.captureRms, st.capturePeak, "");
    if (m_playbackMeters)
        m_playbackMeters->setLevels(st.playbackRms, st.playbackPeak, "");

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
