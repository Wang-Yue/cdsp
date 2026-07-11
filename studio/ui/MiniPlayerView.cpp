#include "ui/MiniPlayerView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QStyleOption>
#include <QPainter>

MiniPlayerView::MiniPlayerView(
    std::shared_ptr<DSPEngineController> dsp,
    std::shared_ptr<AudioSettings> settings,
    std::shared_ptr<MonitoringController> monitoring,
    QWidget* parent
) : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
    m_dsp(dsp), m_settings(settings), m_monitoring(monitoring) {

    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("QWidget#MiniPlayerViewWindow { background-color: rgba(20, 20, 25, 0.85); border-radius: 12px; border: 1px solid rgba(255, 255, 255, 0.15); }");
    setObjectName("MiniPlayerViewWindow");
    resize(360, 120);

    setupUi();
    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, &MiniPlayerView::refreshMeters);
    connect(m_dsp.get(), &DSPEngineController::statusChanged, this, &MiniPlayerView::updateEngineStatus);
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
    m_volSlider->setValue(static_cast<int>(vol * 2.0f));
    m_volValueLabel->setText(QString("%1 dB").arg(vol, 4, 'f', 1));
    m_muteBtn->setText(muted ? "🔇" : "🔊");
}

void MiniPlayerView::updateEngineStatus(ProcessingState state) {
    if (state == ProcessingState::Running) {
        m_playStopBtn->setText("⏹");
        m_playStopBtn->setStyleSheet("color: #ff3b30; font-weight: bold; border-radius: 4px;");
    } else {
        m_playStopBtn->setText("▶");
        m_playStopBtn->setStyleSheet("color: #34c759; font-weight: bold; border-radius: 4px;");
    }
}

void MiniPlayerView::buildMiniPipelineUi() {
    auto layout = qobject_cast<QHBoxLayout*>(m_pipelineMiniCard->layout());
    if (!layout) return;

    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (!m_dsp || !m_dsp->pipelineStore()) return;

    for (const auto& stage : m_dsp->pipelineStore()->stages) {
        auto chip = new QPushButton(QString::fromStdString(stage.name), m_pipelineMiniCard);
        chip->setCheckable(true);
        chip->setChecked(stage.isEnabled);

        auto updateStyle = [chip](bool chk) {
            if (chk) chip->setStyleSheet("background-color: #007aff; color: white; font-size: 10px; border-radius: 4px; padding: 2px 6px;");
            else chip->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; font-size: 10px; border-radius: 4px; padding: 2px 6px;");
        };
        updateStyle(stage.isEnabled);

        QUuid id = stage.id;
        connect(chip, &QPushButton::clicked, [this, id, chip, updateStyle]() {
            auto& stages = m_dsp->pipelineStore()->stages;
            for (auto& st : stages) {
                if (st.id == id) {
                    st.isEnabled = !st.isEnabled;
                    chip->setChecked(st.isEnabled);
                    updateStyle(st.isEnabled);
                    m_dsp->applyConfig();
                    break;
                }
            }
        });
        layout->addWidget(chip);
    }
    layout->addStretch();
}

void MiniPlayerView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(4);

    auto topBar = new QHBoxLayout();
    topBar->setSpacing(4);

    m_playStopBtn = new QPushButton("▶", this);
    m_playStopBtn->setFixedSize(22, 22);
    connect(m_playStopBtn, &QPushButton::clicked, [this]() {
        if (m_dsp->status == ProcessingState::Running) m_dsp->stopEngine();
        else m_dsp->startEngine();
    });
    topBar->addWidget(m_playStopBtn);

    m_muteBtn = new QPushButton("🔊", this);
    m_muteBtn->setFixedSize(22, 22);
    connect(m_muteBtn, &QPushButton::clicked, [this]() {
        Fader f = currentFader();
        bool muted = m_settings->getMuted(f);
        m_dsp->setFaderMute(f, !muted);
        m_muteBtn->setText(!muted ? "🔇" : "🔊");
    });
    topBar->addWidget(m_muteBtn);

    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(-120, 40);
    m_volSlider->setValue(static_cast<int>(m_settings->getVolume(Fader::Main) * 2.0f));

    m_volValueLabel = new QLabel(" 0.0 dB", this);
    m_volValueLabel->setFont(QFont("monospace", 9, QFont::Bold));
    m_volValueLabel->setFixedWidth(50);
    m_volValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    connect(m_volSlider, &QSlider::valueChanged, [this](int val) {
        Fader f = currentFader();
        float db = val / 2.0f;
        m_dsp->setFaderVolume(f, db);
        m_volValueLabel->setText(QString("%1 dB").arg(db, 4, 'f', 1));
    });

    topBar->addWidget(m_volSlider);
    topBar->addWidget(m_volValueLabel);

    // 6 Mode Buttons
    auto pipeBtn = new QPushButton("Pipe", this);
    pipeBtn->setFixedWidth(28);
    connect(pipeBtn, &QPushButton::clicked, [this]() { buildMiniPipelineUi(); m_viewStack->setCurrentIndex(0); });
    topBar->addWidget(pipeBtn);

    auto specBtn = new QPushButton("Spec", this);
    specBtn->setFixedWidth(28);
    connect(specBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(1); });
    topBar->addWidget(specBtn);

    auto vuBtn = new QPushButton("VU", this);
    vuBtn->setFixedWidth(26);
    connect(vuBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(2); });
    topBar->addWidget(vuBtn);

    auto mtrBtn = new QPushButton("Mtr", this);
    mtrBtn->setFixedWidth(26);
    connect(mtrBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(3); });
    topBar->addWidget(mtrBtn);

    auto sgBtn = new QPushButton("SG", this);
    sgBtn->setFixedWidth(24);
    connect(sgBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(4); });
    topBar->addWidget(sgBtn);

    auto vecBtn = new QPushButton("Vec", this);
    vecBtn->setFixedWidth(26);
    connect(vecBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(5); });
    topBar->addWidget(vecBtn);

    auto closeBtn = new QPushButton("✕", this);
    closeBtn->setFixedSize(18, 18);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);
    topBar->addWidget(closeBtn);

    mainLayout->addLayout(topBar);

    m_viewStack = new QStackedWidget(this);

    // Mode 0: Mini Pipeline Chips
    m_pipelineMiniCard = new QWidget(this);
    auto pipeLayout = new QHBoxLayout(m_pipelineMiniCard);
    pipeLayout->setContentsMargins(4, 4, 4, 4);
    pipeLayout->setSpacing(4);
    buildMiniPipelineUi();
    m_viewStack->addWidget(m_pipelineMiniCard);

    // Mode 1: Spectrum
    m_spectrumView = new SpectrumView(m_monitoring ? m_monitoring->spectrumEngine() : nullptr, this);
    m_viewStack->addWidget(m_spectrumView);

    // Mode 2: Analog VU
    m_analogVUView = new AnalogVUMeterView(this);
    if (m_monitoring) m_analogVUView->setLevelState(&m_monitoring->levelState);
    m_viewStack->addWidget(m_analogVUView);

    // Mode 3: Level Meters
    m_metersView = new LevelMeterView(this);
    if (m_monitoring) m_metersView->setLevelState(&m_monitoring->levelState);
    m_viewStack->addWidget(m_metersView);

    // Mode 4: Spectrogram
    m_spectrogramView = new SpectrogramView(m_monitoring ? m_monitoring->spectrogramEngine() : nullptr, this);
    m_viewStack->addWidget(m_spectrogramView);

    // Mode 5: Vector Scope
    m_vectorScopeView = new VectorScopeView(m_monitoring ? m_monitoring->vectorScopeEngine() : nullptr, this);
    m_viewStack->addWidget(m_vectorScopeView);

    mainLayout->addWidget(m_viewStack);
    updateEngineStatus(m_dsp->status);
}

void MiniPlayerView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        hide();
    }
}

void MiniPlayerView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void MiniPlayerView::refreshMeters() {
    const auto& st = m_monitoring->levelState;
    m_metersView->setLevels(st.playbackRms, st.playbackPeak, "Playback");
    float left = !st.playbackPeak.empty() ? st.playbackPeak[0] : -60.0f;
    float right = st.playbackPeak.size() > 1 ? st.playbackPeak[1] : left;
    m_analogVUView->setLevelDB(left, right);
}
