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

Fader MiniPlayerView::currentFader() const {
    int idx = m_faderCombo ? m_faderCombo->currentIndex() : 0;
    switch (idx) {
    case 1: return Fader::Aux1;
    case 2: return Fader::Aux2;
    case 3: return Fader::Aux3;
    case 4: return Fader::Aux4;
    default: return Fader::Main;
    }
}

void MiniPlayerView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
    }
}

void MiniPlayerView::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
    }
}

void MiniPlayerView::onFaderChanged(int index) {
    Q_UNUSED(index);
    Fader f = currentFader();
    float vol = m_settings->getVolume(f);
    bool muted = m_settings->getMuted(f);
    m_volSlider->setValue(static_cast<int>(vol));
    m_volValueLabel->setText(QString("%1 dB").arg(static_cast<int>(vol)));
    m_muteBtn->setText(muted ? "🔇" : "🔊");
}

void MiniPlayerView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    auto topBar = new QHBoxLayout();

    m_faderCombo = new QComboBox(this);
    m_faderCombo->addItems({"Main", "Aux 1", "Aux 2", "Aux 3", "Aux 4"});
    connect(m_faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MiniPlayerView::onFaderChanged);
    topBar->addWidget(m_faderCombo);

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
        Fader f = currentFader();
        bool muted = m_settings->getMuted(f);
        m_dsp->setFaderMute(f, !muted);
        m_muteBtn->setText(!muted ? "🔇" : "🔊");
    });
    topBar->addWidget(m_muteBtn);

    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(-60, 10);
    m_volSlider->setValue(static_cast<int>(m_settings->getVolume(Fader::Main)));

    m_volValueLabel = new QLabel("0 dB", this);
    m_volValueLabel->setFixedWidth(40);
    m_volValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    connect(m_volSlider, &QSlider::valueChanged, [this](int val) {
        Fader f = currentFader();
        m_dsp->setFaderVolume(f, static_cast<float>(val));
        m_volValueLabel->setText(QString("%1 dB").arg(val));
    });

    topBar->addWidget(m_volSlider);
    topBar->addWidget(m_volValueLabel);

    // Mode Buttons
    auto specBtn = new QPushButton("Spec", this);
    connect(specBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(0); });
    topBar->addWidget(specBtn);

    auto vuBtn = new QPushButton("VU", this);
    connect(vuBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(1); });
    topBar->addWidget(vuBtn);

    auto meterBtn = new QPushButton("Meter", this);
    connect(meterBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(2); });
    topBar->addWidget(meterBtn);

    auto sgBtn = new QPushButton("3D", this);
    connect(sgBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(3); });
    topBar->addWidget(sgBtn);

    auto vecBtn = new QPushButton("Vec", this);
    connect(vecBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(4); });
    topBar->addWidget(vecBtn);

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

    m_spectrogramView = new SpectrogramView(this);
    m_viewStack->addWidget(m_spectrogramView);

    m_vectorScopeView = new VectorScopeView(this);
    m_viewStack->addWidget(m_vectorScopeView);

    mainLayout->addWidget(m_viewStack);
}

void MiniPlayerView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        hide();
    }
}

void MiniPlayerView::refreshMeters() {
    const auto& st = m_monitoring->levelState;
    m_metersView->setLevels(st.playbackRms, st.playbackPeak, "Playback");
    float left = !st.playbackPeak.empty() ? st.playbackPeak[0] : -60.0f;
    float right = st.playbackPeak.size() > 1 ? st.playbackPeak[1] : left;
    m_analogVUView->setLevelDB(left, right);
}
