#include "ui/DashboardView.h"

#include "models/PipelineStage.h"

#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
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

        if (!row.slider->isSliderDown()) {
            row.slider->blockSignals(true);
            row.slider->setValue(static_cast<int>(std::round(vol * 2.0f)));
            row.slider->blockSignals(false);
        }

        row.gainValueLabel->setText(QString::asprintf("%+.1f dB", vol));

        row.muteBtn->blockSignals(true);
        row.muteBtn->setChecked(muted);
        row.muteBtn->blockSignals(false);
        row.muteBtn->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
        row.muteBtn->setText("");
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
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto container = new QWidget(scroll);
    container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // 1. Signal Chain Overview Card
    m_pipelineOverviewWidget = new PipelineOverviewWidget(m_dspController, container);
    m_pipelineOverviewWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mainLayout->addWidget(m_pipelineOverviewWidget);

    // 2. Detailed DSP Signal Graph Card
    m_signalGraphCard = new DSPDetailedSignalGraphCard(m_dspController, container);
    m_signalGraphCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mainLayout->addWidget(m_signalGraphCard);

    // 3. Level Meters Card
    m_levelMetersGroup = new QGroupBox("Level Meters", container);
    m_levelMetersGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto levelGroupLayout = new QVBoxLayout(m_levelMetersGroup);
    levelGroupLayout->setContentsMargins(12, 12, 12, 12);
    levelGroupLayout->setSpacing(8);

    m_levelMetersCard = new LevelMetersCard(m_monitoring, m_levelMetersGroup);
    m_levelMetersCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    levelGroupLayout->addWidget(m_levelMetersCard);
    mainLayout->addWidget(m_levelMetersGroup);

    // 4. Volume Faders Card
    m_faderGroup = new QGroupBox("Volume Faders", container);
    m_faderGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto faderGridLayout = new QGridLayout(m_faderGroup);
    faderGridLayout->setContentsMargins(12, 12, 12, 12);
    faderGridLayout->setHorizontalSpacing(12);
    faderGridLayout->setVerticalSpacing(8);

    struct FaderInfo {
        Fader fader;
        QString name;
    };
    const std::vector<FaderInfo> faders = {{Fader::Main, "Main"},
                                           {Fader::Aux1, "Aux 1"},
                                           {Fader::Aux2, "Aux 2"},
                                           {Fader::Aux3, "Aux 3"},
                                           {Fader::Aux4, "Aux 4"}};

    m_faderRows.clear();
    int row = 0;
    for (const auto& info : faders) {
        auto nameLbl = new QLabel(info.name, m_faderGroup);
        nameLbl->setFixedWidth(70);
        QFont nameFont = nameLbl->font();
        nameFont.setWeight(QFont::Medium);
        nameLbl->setFont(nameFont);

        auto muteBtn = new QPushButton(m_faderGroup);
        muteBtn->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
        muteBtn->setCheckable(true);
        muteBtn->setFixedSize(28, 28);
        muteBtn->setToolTip(QString("Mute %1").arg(info.name));

        auto slider = new QSlider(Qt::Horizontal, m_faderGroup);
        slider->setRange(-120, 40); // -60.0 dB to +20.0 dB

        auto gainLbl = new QLabel(" 0.0 dB", m_faderGroup);
        QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        monoFont.setPointSize(11);
        gainLbl->setFont(monoFont);
        gainLbl->setFixedWidth(65);
        gainLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        Fader f = info.fader;
        connect(muteBtn, &QPushButton::clicked, [this, f]() {
            if (m_dspController && m_dspController->settings()) {
                bool currentMute = m_dspController->settings()->getMuted(f);
                m_dspController->setFaderMute(f, !currentMute);
                updateFaderUi();
            }
        });

        connect(slider, &QSlider::valueChanged, [this, f](int val) {
            if (m_dspController) {
                float db = val / 2.0f;
                m_dspController->setFaderVolume(f, db, true);
                updateFaderUi();
            }
        });

        faderGridLayout->addWidget(nameLbl, row, 0);
        faderGridLayout->addWidget(muteBtn, row, 1);
        faderGridLayout->addWidget(slider, row, 2);
        faderGridLayout->addWidget(gainLbl, row, 3);

        m_faderRows.push_back({f, nameLbl, muteBtn, slider, gainLbl});
        ++row;
    }
    faderGridLayout->setColumnStretch(2, 1);
    mainLayout->addWidget(m_faderGroup);

    // 5. Analog VU Card
    m_analogVUGroup = new QGroupBox("Analog VU", container);
    m_analogVUGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto vuLayout = new QVBoxLayout(m_analogVUGroup);
    vuLayout->setContentsMargins(12, 12, 12, 12);
    vuLayout->setSpacing(10);

    m_analogVUView = new AnalogVUMeterView(m_analogVUGroup);
    m_analogVUView->setLevelState(&m_monitoring->levelState);
    m_analogVUView->setFixedHeight(200);
    vuLayout->addWidget(m_analogVUView);

    mainLayout->addWidget(m_analogVUGroup);

    // 6. Spectrum Card
    m_spectrumGroup = new QGroupBox("Spectrum", container);
    m_spectrumGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto specLayout = new QVBoxLayout(m_spectrumGroup);
    specLayout->setContentsMargins(12, 12, 12, 12);
    specLayout->setSpacing(8);
    m_spectrumView = new SpectrumView(m_spectrumEngine, m_spectrumGroup);
    m_spectrumView->setFixedHeight(160);
    specLayout->addWidget(m_spectrumView);
    mainLayout->addWidget(m_spectrumGroup);

    // 7. Spectrogram Card
    m_spectrogramGroup = new QGroupBox("Spectrogram", container);
    m_spectrogramGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto spectroLayout = new QVBoxLayout(m_spectrogramGroup);
    spectroLayout->setContentsMargins(12, 12, 12, 12);
    spectroLayout->setSpacing(8);
    m_spectrogramView = new SpectrogramView(m_spectrogramEngine, m_spectrogramGroup);
    m_spectrogramView->setFixedHeight(480);
    spectroLayout->addWidget(m_spectrogramView);
    mainLayout->addWidget(m_spectrogramGroup);

    // 8. Vector Scope Card
    m_vectorScopeGroup = new QGroupBox("Vector Scope", container);
    m_vectorScopeGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto vecLayout = new QVBoxLayout(m_vectorScopeGroup);
    vecLayout->setContentsMargins(12, 12, 12, 12);
    vecLayout->setSpacing(8);
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
    if (m_levelMetersCard)
        m_levelMetersCard->update();

    if (m_analogVUView)
        m_analogVUView->setLevels(st.playbackRms);

    if (m_spectrumEngine && m_spectrumView)
        m_spectrumView->setSpectrum(m_spectrumEngine->data);
    if (m_spectrogramEngine && m_spectrogramView)
        m_spectrogramView->setHistory(m_spectrogramEngine->history, m_spectrogramEngine->show3D);
    if (m_vectorScopeEngine && m_vectorScopeView)
        m_vectorScopeView->setSamples(m_vectorScopeEngine->samples, m_vectorScopeEngine->showParticles);
}
