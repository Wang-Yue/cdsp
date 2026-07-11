#include "ui/DashboardView.h"
#include "ui/StyleTheme.h"
#include "models/PipelineStage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QSlider>
#include <QFont>
#include <cmath>

DashboardView::DashboardView(
    std::shared_ptr<MonitoringController> monitoring,
    std::shared_ptr<DSPEngineController> dspController,
    std::shared_ptr<SpectrumEngine> spectrumEngine,
    std::shared_ptr<SpectrogramEngine> spectrogramEngine,
    std::shared_ptr<VectorScopeEngine> vectorScopeEngine,
    QWidget* parent
) : QWidget(parent),
    m_monitoring(monitoring),
    m_dspController(dspController),
    m_spectrumEngine(spectrumEngine),
    m_spectrogramEngine(spectrogramEngine),
    m_vectorScopeEngine(vectorScopeEngine) {
    setupUi();

    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, &DashboardView::refreshMeters);

    if (m_dspController && m_dspController->settings()) {
        connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this, &DashboardView::updateVisibility);
        connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this, &DashboardView::updateFaderUi);
    }
    updateVisibility();
    updateFaderUi();
}

void DashboardView::updateVisibility() {
    if (!m_dspController || !m_dspController->settings()) return;
    auto s = m_dspController->settings();
    if (m_levelMetersGroup) m_levelMetersGroup->setVisible(s->showLevelMetersInDashboard);
    if (m_analogVUGroup) m_analogVUGroup->setVisible(s->showAnalogVUInDashboard);
    if (m_spectrumGroup) m_spectrumGroup->setVisible(s->showSpectrumInDashboard);
    if (m_spectrogramGroup) m_spectrogramGroup->setVisible(s->showSpectrogramInDashboard);
    if (m_vectorScopeGroup) m_vectorScopeGroup->setVisible(s->showVectorScopeInDashboard);
}

void DashboardView::updateFaderUi() {
    if (!m_dspController || !m_dspController->settings()) return;
    auto s = m_dspController->settings();

    for (auto& row : m_faderRows) {
        float vol = s->getVolume(row.fader);
        bool muted = s->getMuted(row.fader);

        row.slider->blockSignals(true);
        row.slider->setValue(static_cast<int>(std::round(vol * 2.0f)));
        row.slider->blockSignals(false);

        row.gainValueLabel->setText(QString::asprintf("%+.1f dB", vol));
        if (vol > 0.0f) {
            row.gainValueLabel->setStyleSheet("font-family: monospace; font-weight: bold; color: #ff3b30; min-width: 75px;");
        } else {
            row.gainValueLabel->setStyleSheet("font-family: monospace; font-weight: bold; color: #34c759; min-width: 75px;");
        }

        row.muteBtn->blockSignals(true);
        row.muteBtn->setChecked(muted);
        row.muteBtn->blockSignals(false);

        if (muted) {
            row.muteBtn->setText("🔇 Muted");
            row.muteBtn->setStyleSheet("background-color: #ff3b30; color: white; padding: 4px 8px; border-radius: 4px; font-weight: bold;");
        } else {
            row.muteBtn->setText("🔊 Mute");
            row.muteBtn->setStyleSheet("background-color: #3a3a3c; color: white; padding: 4px 8px; border-radius: 4px;");
        }
    }
}

void DashboardView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setSpacing(16);

    // 1. Signal Chain Overview Card (Horizontal Scrollable)
    auto chainGroup = new QGroupBox("Signal Chain Overview", container);
    auto chainGroupLayout = new QVBoxLayout(chainGroup);
    chainGroupLayout->setContentsMargins(4, 4, 4, 4);

    auto chainScroll = new QScrollArea(chainGroup);
    chainScroll->setWidgetResizable(true);
    chainScroll->setFrameShape(QFrame::NoFrame);
    chainScroll->setFixedHeight(60);

    auto chainWidget = new QWidget(chainScroll);
    auto chainLayout = new QHBoxLayout(chainWidget);
    chainLayout->setContentsMargins(8, 8, 8, 8);
    chainLayout->setSpacing(8);

    const auto& devConf = m_dspController->settings()->deviceConfig;
    QString capDevName = QString::fromStdString(devConf.capture.coreAudio.device.value_or("System Default"));
    QString playDevName = QString::fromStdString(devConf.playback.coreAudio.device.value_or("System Default"));

    auto capChip = new QPushButton(QString("🎤 Input: %1").arg(capDevName), chainWidget);
    capChip->setStyleSheet("background-color: #3a3a3c; color: #ffffff; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
    chainLayout->addWidget(capChip);

    auto arrow1 = new QLabel("➔", chainWidget); arrow1->setStyleSheet("color: #8e8e93; font-weight: bold;");
    chainLayout->addWidget(arrow1);

    bool resampEnabled = m_dspController->settings()->resamplerEnabled;
    auto resampChip = new QPushButton(QString("🔄 Resampler (%1)").arg(resampEnabled ? "Active" : "Bypassed"), chainWidget);
    resampChip->setCheckable(true);
    resampChip->setChecked(resampEnabled);
    if (resampEnabled) resampChip->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
    else resampChip->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; border-radius: 6px; padding: 6px 12px;");

    connect(resampChip, &QPushButton::clicked, [this, resampChip]() {
        bool enabled = !m_dspController->settings()->resamplerEnabled;
        m_dspController->settings()->resamplerEnabled = enabled;
        resampChip->setChecked(enabled);
        resampChip->setText(QString("🔄 Resampler (%1)").arg(enabled ? "Active" : "Bypassed"));
        if (enabled) resampChip->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
        else resampChip->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; border-radius: 6px; padding: 6px 12px;");
        m_dspController->applyConfig();
    });
    chainLayout->addWidget(resampChip);

    auto arrow2 = new QLabel("➔", chainWidget); arrow2->setStyleSheet("color: #8e8e93; font-weight: bold;");
    chainLayout->addWidget(arrow2);

    for (size_t i = 0; i < m_dspController->pipelineStore()->stages.size(); ++i) {
        const auto& st = m_dspController->pipelineStore()->stages[i];
        std::string icon = stageTypeToIcon(st.type);
        auto stChip = new QPushButton(QString("%1 %2").arg(QString::fromStdString(icon)).arg(QString::fromStdString(st.name)), chainWidget);
        stChip->setCheckable(true);
        stChip->setChecked(st.isEnabled);
        if (st.isEnabled) stChip->setStyleSheet("background-color: #34c759; color: white; font-weight: bold; border-radius: 6px; padding: 6px 10px;");
        else stChip->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; border-radius: 6px; padding: 6px 10px;");

        connect(stChip, &QPushButton::clicked, [this, i, stChip]() {
            m_dspController->pipelineStore()->stages[i].isEnabled = !m_dspController->pipelineStore()->stages[i].isEnabled;
            bool enabled = m_dspController->pipelineStore()->stages[i].isEnabled;
            stChip->setChecked(enabled);
            if (enabled) stChip->setStyleSheet("background-color: #34c759; color: white; font-weight: bold; border-radius: 6px; padding: 6px 10px;");
            else stChip->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; border-radius: 6px; padding: 6px 10px;");
            m_dspController->applyConfig();
        });
        chainLayout->addWidget(stChip);

        if (i + 1 < m_dspController->pipelineStore()->stages.size()) {
            auto arr = new QLabel("➔", chainWidget); arr->setStyleSheet("color: #8e8e93;");
            chainLayout->addWidget(arr);
        }
    }

    auto arrow3 = new QLabel("➔", chainWidget); arrow3->setStyleSheet("color: #8e8e93; font-weight: bold;");
    chainLayout->addWidget(arrow3);

    auto playChip = new QPushButton(QString("🔊 Output: %1").arg(playDevName), chainWidget);
    playChip->setStyleSheet("background-color: #3a3a3c; color: #ffffff; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
    chainLayout->addWidget(playChip);

    chainScroll->setWidget(chainWidget);
    chainGroupLayout->addWidget(chainScroll);
    mainLayout->addWidget(chainGroup);

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

    struct FaderInfo { Fader fader; QString name; };
    std::vector<FaderInfo> faders = {
        {Fader::Main, "Main"},
        {Fader::Aux1, "Aux 1"},
        {Fader::Aux2, "Aux 2"},
        {Fader::Aux3, "Aux 3"},
        {Fader::Aux4, "Aux 4"}
    };

    m_faderRows.clear();
    for (const auto& info : faders) {
        auto rowLayout = new QHBoxLayout();

        auto nameLbl = new QLabel(info.name, faderGroup);
        nameLbl->setFixedWidth(80);
        nameLbl->setFont(QFont("System", 13, QFont::DemiBold));

        auto muteBtn = new QPushButton("🔊 Mute", faderGroup);
        muteBtn->setCheckable(true);
        muteBtn->setFixedWidth(90);

        auto slider = new QSlider(Qt::Horizontal, faderGroup);
        slider->setRange(-120, 40); // -60.0 dB to +20.0 dB

        auto gainLbl = new QLabel(" 0.0 dB", faderGroup);
        gainLbl->setFont(QFont("monospace", 11, QFont::Bold));
        gainLbl->setFixedWidth(75);
        gainLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        Fader f = info.fader;
        connect(muteBtn, &QPushButton::clicked, [this, f]() {
            bool currentMute = m_dspController->settings()->getMuted(f);
            m_dspController->setFaderMute(f, !currentMute);
            updateFaderUi();
        });

        connect(slider, &QSlider::valueChanged, [this, f](int val) {
            float db = val / 2.0f;
            m_dspController->setFaderVolume(f, db);
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
    m_analogVUGroup = new QGroupBox("Analog VU Meter", container);
    auto vuLayout = new QVBoxLayout(m_analogVUGroup);
    m_analogVUView = new AnalogVUMeterView(m_analogVUGroup);
    m_analogVUView->setLevelState(&m_monitoring->levelState);
    vuLayout->addWidget(m_analogVUView);
    mainLayout->addWidget(m_analogVUGroup);

    // 5. Spectrum Card
    m_spectrumGroup = new QGroupBox("Spectrum Analyzer", container);
    auto specLayout = new QVBoxLayout(m_spectrumGroup);
    m_spectrumView = new SpectrumView(m_spectrumEngine, m_spectrumGroup);
    m_spectrumView->setFixedHeight(220);
    specLayout->addWidget(m_spectrumView);
    mainLayout->addWidget(m_spectrumGroup);

    // 6. Spectrogram Card
    m_spectrogramGroup = new QGroupBox("Spectroscope", container);
    auto spectroLayout = new QVBoxLayout(m_spectrogramGroup);
    m_spectrogramView = new SpectrogramView(m_spectrogramEngine, m_spectrogramGroup);
    m_spectrogramView->setFixedHeight(360);
    spectroLayout->addWidget(m_spectrogramView);
    mainLayout->addWidget(m_spectrogramGroup);

    // 7. Vector Scope Card
    m_vectorScopeGroup = new QGroupBox("Vector Scope", container);
    auto vecLayout = new QVBoxLayout(m_vectorScopeGroup);
    m_vectorScopeView = new VectorScopeView(m_vectorScopeEngine, m_vectorScopeGroup);
    m_vectorScopeView->setFixedHeight(400);
    vecLayout->addWidget(m_vectorScopeView);
    mainLayout->addWidget(m_vectorScopeGroup);

    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void DashboardView::refreshMeters() {
    const auto& st = m_monitoring->levelState;
    if (m_captureMeters) m_captureMeters->setLevels(st.captureRms, st.capturePeak, "Capture Levels");
    if (m_playbackMeters) m_playbackMeters->setLevels(st.playbackRms, st.playbackPeak, "Playback Levels");

    float leftDB = !st.playbackRms.empty() ? st.playbackRms[0] : -60.0f;
    float rightDB = st.playbackRms.size() > 1 ? st.playbackRms[1] : leftDB;
    if (m_analogVUView) m_analogVUView->setLevelDB(leftDB, rightDB);

    if (m_spectrumEngine && m_spectrumView) m_spectrumView->setSpectrum(m_spectrumEngine->data);
    if (m_spectrogramEngine && m_spectrogramView) m_spectrogramView->setHistory(m_spectrogramEngine->history, m_spectrogramEngine->show3D);
    if (m_vectorScopeEngine && m_vectorScopeView) m_vectorScopeView->setSamples(m_vectorScopeEngine->samples, m_vectorScopeEngine->showParticles);
}
