#include "ui/DashboardView.h"
#include "ui/StyleTheme.h"
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
    m_mainFaderSlider->setValue(static_cast<int>(vol));
    m_mainFaderSlider->blockSignals(false);

    m_volValueLabel->setText(QString("%1 dB").arg(static_cast<int>(vol)));
    m_mainMuteBtn->setChecked(muted);
    m_mainMuteBtn->setText(muted ? "Unmute" : "Mute");

    auto updateBtnStyle = [this](QPushButton* btn, Fader f) {
        if (m_activeFader == f) {
            btn->setStyleSheet("background-color: #007aff; color: white; font-weight: bold;");
        } else {
            btn->setStyleSheet("background-color: #e5e5ea; color: #000000;");
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
    float target = std::clamp(cur + step, -60.0f, 10.0f);
    m_dspController->setFaderVolume(m_activeFader, target);
    updateFaderUi();
}

void DashboardView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    // 1. Signal Chain Overview Card
    auto chainGroup = new QGroupBox("Signal Chain Overview", container);
    auto chainLayout = new QHBoxLayout(chainGroup);
    chainLayout->setContentsMargins(12, 12, 12, 12);
    chainLayout->setSpacing(8);

    auto capChip = new QPushButton("🎤 Input: System Default", chainGroup);
    capChip->setStyleSheet("background-color: #e5e5ea; color: #1c1c1e; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
    chainLayout->addWidget(capChip);

    auto arrow1 = new QLabel("➔", chainGroup); arrow1->setStyleSheet("color: #8e8e93; font-weight: bold;");
    chainLayout->addWidget(arrow1);

    auto resampChip = new QPushButton("🔄 Resampler (AsyncSinc)", chainGroup);
    resampChip->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
    chainLayout->addWidget(resampChip);

    auto arrow2 = new QLabel("➔", chainGroup); arrow2->setStyleSheet("color: #8e8e93; font-weight: bold;");
    chainLayout->addWidget(arrow2);

    for (size_t i = 0; i < m_dspController->pipelineStore()->stages.size(); ++i) {
        const auto& st = m_dspController->pipelineStore()->stages[i];
        auto stChip = new QPushButton(QString("⚙️ %1").arg(QString::fromStdString(st.name)), chainGroup);
        stChip->setCheckable(true);
        stChip->setChecked(st.isEnabled);
        if (st.isEnabled) stChip->setStyleSheet("background-color: #34c759; color: white; font-weight: bold; border-radius: 6px; padding: 6px 10px;");
        else stChip->setStyleSheet("background-color: #e5e5ea; color: #8e8e93; border-radius: 6px; padding: 6px 10px;");

        connect(stChip, &QPushButton::clicked, [this, i]() {
            m_dspController->pipelineStore()->stages[i].isEnabled = !m_dspController->pipelineStore()->stages[i].isEnabled;
            m_dspController->applyConfig();
        });
        chainLayout->addWidget(stChip);

        if (i + 1 < m_dspController->pipelineStore()->stages.size()) {
            auto arr = new QLabel("➔", chainGroup); arr->setStyleSheet("color: #8e8e93;");
            chainLayout->addWidget(arr);
        }
    }

    auto arrow3 = new QLabel("➔", chainGroup); arrow3->setStyleSheet("color: #8e8e93; font-weight: bold;");
    chainLayout->addWidget(arrow3);

    auto playChip = new QPushButton("🔊 Output: System Default", chainGroup);
    playChip->setStyleSheet("background-color: #e5e5ea; color: #1c1c1e; font-weight: bold; border-radius: 6px; padding: 6px 12px;");
    chainLayout->addWidget(playChip);
    chainLayout->addStretch();

    mainLayout->addWidget(chainGroup);

    // Header Fader Bar
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

    // Slider & Controls Bar
    auto controlsLayout = new QHBoxLayout();
    controlsLayout->addWidget(new QLabel("Fader Gain:", faderGroup));

    m_mainFaderSlider = new QSlider(Qt::Horizontal, faderGroup);
    m_mainFaderSlider->setRange(-60, 10);
    m_mainFaderSlider->setValue(0);
    connect(m_mainFaderSlider, &QSlider::valueChanged, [this](int val) {
        m_dspController->setFaderVolume(m_activeFader, static_cast<float>(val));
        m_volValueLabel->setText(QString("%1 dB").arg(val));
    });
    controlsLayout->addWidget(m_mainFaderSlider);

    m_volValueLabel = new QLabel("0 dB", faderGroup);
    m_volValueLabel->setFixedWidth(50);
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
