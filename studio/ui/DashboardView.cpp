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

void DashboardView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // Header Fader Bar
    auto faderGroup = new QGroupBox("Master Controls", container);
    auto faderLayout = new QHBoxLayout(faderGroup);

    faderLayout->addWidget(new QLabel("Main Volume:", faderGroup));
    m_mainFaderSlider = new QSlider(Qt::Horizontal, faderGroup);
    m_mainFaderSlider->setRange(-60, 10);
    m_mainFaderSlider->setValue(0);
    connect(m_mainFaderSlider, &QSlider::valueChanged, [this](int val) {
        m_dspController->setFaderVolume(Fader::Main, static_cast<float>(val));
    });
    faderLayout->addWidget(m_mainFaderSlider);

    m_mainMuteBtn = new QPushButton("Mute", faderGroup);
    m_mainMuteBtn->setCheckable(true);
    connect(m_mainMuteBtn, &QPushButton::toggled, [this](bool checked) {
        m_dspController->setFaderMute(Fader::Main, checked);
    });
    faderLayout->addWidget(m_mainMuteBtn);

    mainLayout->addWidget(faderGroup);

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
