#include "ui/DashboardView.h"
#include "ui/StyleTheme.h"
#include "models/PipelineStage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>

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
}

void DashboardView::updateFaderUi() {
    float vol = m_dspController->settings()->getVolume(m_activeFader);
    bool muted = m_dspController->settings()->getMuted(m_activeFader);

    m_mainFaderSlider->blockSignals(true);
    m_mainFaderSlider->setValue(static_cast<int>(vol * 2.0f));
    m_mainFaderSlider->blockSignals(false);

    m_volValueLabel->setText(QString("%1 dB").arg(vol, 4, 'f', 1));
    m_mainMuteBtn->setChecked(muted);
    m_mainMuteBtn->setText(muted ? "Unmute" : "Mute");

    auto updateBtnStyle = [this](QPushButton* btn, Fader f) {
        if (m_activeFader == f) {
            btn->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; border-radius: 4px;");
        } else {
            btn->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; border-radius: 4px;");
        }
    };
    updateBtnStyle(m_faderMainBtn, Fader::Main);
    updateBtnStyle(m_faderAux1Btn, Fader::Aux1);
    updateBtnStyle(m_faderAux2Btn, Fader::Aux2);
    updateBtnStyle(m_faderAux3Btn, Fader::Aux3);
    updateBtnStyle(m_faderAux4Btn, Fader::Aux4);
}

void DashboardView::setFaderVolumeStep(float step) {
    float cur = m_dspController->settings()->getVolume(m_activeFader);
    float target = std::clamp(cur + step, -60.0f, 20.0f);
    m_dspController->setFaderVolume(m_activeFader, target);
    updateFaderUi();
}

void DashboardView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);

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

    // Master Controls & Output Faders Bar
    auto faderGroup = new QGroupBox("Master Controls & Output Faders", container);
    auto faderVLayout = new QVBoxLayout(faderGroup);

    // Fader Selector Bar
    auto selectorLayout = new QHBoxLayout();
    m_faderMainBtn = new QPushButton("Main", faderGroup);
    m_faderAux1Btn = new QPushButton("Aux 1", faderGroup);
    m_faderAux2Btn = new QPushButton("Aux 2", faderGroup);
    m_faderAux3Btn = new QPushButton("Aux 3", faderGroup);
    m_faderAux4Btn = new QPushButton("Aux 4", faderGroup);

    connect(m_faderMainBtn, &QPushButton::clicked, [this]() { m_activeFader = Fader::Main; updateFaderUi(); });
    connect(m_faderAux1Btn, &QPushButton::clicked, [this]() { m_activeFader = Fader::Aux1; updateFaderUi(); });
    connect(m_faderAux2Btn, &QPushButton::clicked, [this]() { m_activeFader = Fader::Aux2; updateFaderUi(); });
    connect(m_faderAux3Btn, &QPushButton::clicked, [this]() { m_activeFader = Fader::Aux3; updateFaderUi(); });
    connect(m_faderAux4Btn, &QPushButton::clicked, [this]() { m_activeFader = Fader::Aux4; updateFaderUi(); });

    selectorLayout->addWidget(m_faderMainBtn);
    selectorLayout->addWidget(m_faderAux1Btn);
    selectorLayout->addWidget(m_faderAux2Btn);
    selectorLayout->addWidget(m_faderAux3Btn);
    selectorLayout->addWidget(m_faderAux4Btn);
    selectorLayout->addStretch();

    faderVLayout->addLayout(selectorLayout);

    // Slider & Controls Bar (-60 to +20 dB, step 0.5)
    auto controlsLayout = new QHBoxLayout();
    controlsLayout->addWidget(new QLabel("Fader Gain:", faderGroup));

    m_mainFaderSlider = new QSlider(Qt::Horizontal, faderGroup);
    m_mainFaderSlider->setRange(-120, 40);
    m_mainFaderSlider->setValue(0);
    connect(m_mainFaderSlider, &QSlider::valueChanged, [this](int val) {
        float db = val / 2.0f;
        m_dspController->setFaderVolume(m_activeFader, db);
        m_volValueLabel->setText(QString("%1 dB").arg(db, 4, 'f', 1));
    });
    controlsLayout->addWidget(m_mainFaderSlider, 1);

    m_volValueLabel = new QLabel(" 0.0 dB", faderGroup);
    m_volValueLabel->setFont(QFont("monospace", 10, QFont::Bold));
    m_volValueLabel->setFixedWidth(60);
    m_volValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    controlsLayout->addWidget(m_volValueLabel);

    // Step adjustment buttons
    auto btnMinus1 = new QPushButton("-1 dB", faderGroup);
    auto btnMinusHalf = new QPushButton("-0.5 dB", faderGroup);
    auto btnPlusHalf = new QPushButton("+0.5 dB", faderGroup);
    auto btnPlus1 = new QPushButton("+1 dB", faderGroup);

    connect(btnMinus1, &QPushButton::clicked, [this]() { setFaderVolumeStep(-1.0f); });
    connect(btnMinusHalf, &QPushButton::clicked, [this]() { setFaderVolumeStep(-0.5f); });
    connect(btnPlusHalf, &QPushButton::clicked, [this]() { setFaderVolumeStep(0.5f); });
    connect(btnPlus1, &QPushButton::clicked, [this]() { setFaderVolumeStep(1.0f); });

    controlsLayout->addWidget(btnMinus1);
    controlsLayout->addWidget(btnMinusHalf);
    controlsLayout->addWidget(btnPlusHalf);
    controlsLayout->addWidget(btnPlus1);

    m_mainMuteBtn = new QPushButton("Mute", faderGroup);
    m_mainMuteBtn->setCheckable(true);
    connect(m_mainMuteBtn, &QPushButton::toggled, [this](bool checked) {
        m_dspController->setFaderMute(m_activeFader, checked);
        m_mainMuteBtn->setText(checked ? "Unmute" : "Mute");
    });
    controlsLayout->addWidget(m_mainMuteBtn);

    faderVLayout->addLayout(controlsLayout);
    mainLayout->addWidget(faderGroup);

    updateFaderUi();

    // Monitoring Cards Grid
    auto grid = new QGridLayout();
    grid->setSpacing(16);

    m_captureMeters = new LevelMeterView(container);
    grid->addWidget(m_captureMeters, 0, 0);

    m_playbackMeters = new LevelMeterView(container);
    grid->addWidget(m_playbackMeters, 0, 1);

    m_analogVUView = new AnalogVUMeterView(container);
    grid->addWidget(m_analogVUView, 1, 0);

    m_spectrumView = new SpectrumView(m_spectrumEngine, container);
    grid->addWidget(m_spectrumView, 1, 1);

    m_spectrogramView = new SpectrogramView(m_spectrogramEngine, container);
    grid->addWidget(m_spectrogramView, 2, 0);

    m_vectorScopeView = new VectorScopeView(m_vectorScopeEngine, container);
    grid->addWidget(m_vectorScopeView, 2, 1);

    mainLayout->addLayout(grid);
    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void DashboardView::refreshMeters() {
    const auto& st = m_monitoring->levelState;
    m_captureMeters->setLevels(st.captureRms, st.capturePeak, "Capture Levels");
    m_playbackMeters->setLevels(st.playbackRms, st.playbackPeak, "Playback Levels");

    float leftDB = !st.playbackRms.empty() ? st.playbackRms[0] : -60.0f;
    float rightDB = st.playbackRms.size() > 1 ? st.playbackRms[1] : leftDB;
    m_analogVUView->setLevelDB(leftDB, rightDB);

    if (m_spectrumEngine) m_spectrumView->setSpectrum(m_spectrumEngine->data);
    if (m_spectrogramEngine) m_spectrogramView->setHistory(m_spectrogramEngine->history, m_spectrogramEngine->show3D);
    if (m_vectorScopeEngine) m_vectorScopeView->setSamples(m_vectorScopeEngine->samples, m_vectorScopeEngine->showParticles);
}
