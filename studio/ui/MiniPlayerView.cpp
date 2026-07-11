#include "ui/MiniPlayerView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>

MiniPlayerView::MiniPlayerView(
    std::shared_ptr<DSPEngineController> dsp,
    std::shared_ptr<AudioSettings> settings,
    std::shared_ptr<MonitoringController> monitoring,
    QWidget* parent
) : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
    m_dsp(dsp), m_settings(settings), m_monitoring(monitoring) {

    setAttribute(Qt::WA_TranslucentBackground);
    resize(360, 180);

    setupUi();
    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, &MiniPlayerView::refreshMeters);
}

void MiniPlayerView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    auto topBar = new QHBoxLayout();

    m_playStopBtn = new QPushButton("▶", this);
    m_playStopBtn->setFixedSize(24, 24);
    connect(m_playStopBtn, &QPushButton::clicked, [this]() {
        if (m_dsp->status == ProcessingState::Running) m_dsp->stopEngine();
        else m_dsp->startEngine();
    });
    topBar->addWidget(m_playStopBtn);

    m_muteBtn = new QPushButton("🔊", this);
    m_muteBtn->setFixedSize(24, 24);
    connect(m_muteBtn, &QPushButton::clicked, [this]() {
        bool muted = m_settings->getMuted(Fader::Main);
        m_dsp->setFaderMute(Fader::Main, !muted);
        m_muteBtn->setText(!muted ? "🔇" : "🔊");
    });
    topBar->addWidget(m_muteBtn);

    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(-60, 10);
    m_volSlider->setValue(static_cast<int>(m_settings->getVolume(Fader::Main)));
    connect(m_volSlider, &QSlider::valueChanged, [this](int val) {
        m_dsp->setFaderVolume(Fader::Main, static_cast<float>(val));
    });
    topBar->addWidget(m_volSlider);

    // Mode Buttons
    auto specBtn = new QPushButton("Spec", this);
    connect(specBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(0); });
    topBar->addWidget(specBtn);

    auto metersBtn = new QPushButton("VU", this);
    connect(metersBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(1); });
    topBar->addWidget(metersBtn);

    auto closeBtn = new QPushButton("✕", this);
    closeBtn->setFixedSize(20, 20);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);
    topBar->addWidget(closeBtn);

    mainLayout->addLayout(topBar);

    m_viewStack = new QStackedWidget(this);

    m_spectrumView = new SpectrumView(this);
    m_viewStack->addWidget(m_spectrumView);

    m_analogVUView = new AnalogVUMeterView(this);
    m_viewStack->addWidget(m_analogVUView);

    m_metersView = new LevelMeterView(this);
    m_viewStack->addWidget(m_metersView);

    mainLayout->addWidget(m_viewStack);
}

void MiniPlayerView::refreshMeters() {
    const auto& st = m_monitoring->levelState;
    m_metersView->setLevels(st.playbackRms, st.playbackPeak, "Playback");
    float left = !st.playbackPeak.empty() ? st.playbackPeak[0] : -60.0f;
    float right = st.playbackPeak.size() > 1 ? st.playbackPeak[1] : left;
    m_analogVUView->setLevelDB(left, right);
}
