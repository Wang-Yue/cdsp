#include "ui/MiniPlayerView.h"

#include "ui/StyleTheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWindow>

#ifdef Q_OS_MAC
#include <objc/message.h>
#include <objc/runtime.h>

static void setMacFloatingPanelProperties(QWidget* widget) {
    if (auto window = widget->windowHandle()) {
        void* nsWindow = reinterpret_cast<void*>(window->winId());
        if (nsWindow) {
            unsigned long behavior = (1UL << 0) | (1UL << 8) | (1UL << 10);
            reinterpret_cast<void (*)(void*, SEL, unsigned long)>(objc_msgSend)(
                nsWindow, sel_registerName("setCollectionBehavior:"), behavior);
            reinterpret_cast<void (*)(void*, SEL, long)>(objc_msgSend)(nsWindow, sel_registerName("setLevel:"), 1000L);
        }
    }
}
#endif

MiniPlayerView::MiniPlayerView(std::shared_ptr<DSPEngineController> dsp, std::shared_ptr<AudioSettings> settings,
                               std::shared_ptr<MonitoringController> monitoring, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_dsp(dsp),
      m_settings(settings), m_monitoring(monitoring) {

    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("QWidget#MiniPlayerViewWindow { background-color: rgba(20, 20, 25, 0.85); border-radius: 12px; "
                  "border: 1px solid rgba(255, 255, 255, 0.15); }");
    setObjectName("MiniPlayerViewWindow");
    setMinimumSize(200, 80);
    setMaximumSize(1000, 300);
    resize(320, 90);

    setupUi();
    connect(m_monitoring.get(), &MonitoringController::levelsUpdated, this, &MiniPlayerView::refreshMeters);
    connect(m_dsp.get(), &DSPEngineController::statusChanged, this, &MiniPlayerView::updateEngineStatus);
    if (m_dsp && m_dsp->pipelineStore()) {
        connect(m_dsp->pipelineStore().get(), &PipelineStore::pipelineChanged, this,
                [this]() { buildMiniPipelineUi(); });
    }
    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::settingsChanged, this, [this]() { onFaderChanged(0); });
    }
}

void MiniPlayerView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
#ifdef Q_OS_MAC
    setMacFloatingPanelProperties(this);
#endif
}

Fader MiniPlayerView::currentFader() const {
    int idx = m_faderCombo ? m_faderCombo->currentIndex() : 0;
    switch (idx) {
    case 1:
        return Fader::Aux1;
    case 2:
        return Fader::Aux2;
    case 3:
        return Fader::Aux3;
    case 4:
        return Fader::Aux4;
    default:
        return Fader::Main;
    }
}

void MiniPlayerView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QWidget* child = childAt(event->position().toPoint());
        if (!child || child == this || child == m_pipelineMiniCard) {
            m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
            m_isDragging = true;
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void MiniPlayerView::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void MiniPlayerView::mouseReleaseEvent(QMouseEvent* event) {
    m_isDragging = false;
    QWidget::mouseReleaseEvent(event);
}

void MiniPlayerView::onFaderChanged(int index) {
    Q_UNUSED(index);
    if (!m_settings || !m_volSlider || !m_volValueLabel || !m_muteBtn)
        return;
    Fader f = currentFader();
    float vol = m_settings->getVolume(f);
    bool muted = m_settings->getMuted(f);
    m_volSlider->blockSignals(true);
    m_volSlider->setValue(static_cast<int>(vol * 2.0f));
    m_volSlider->blockSignals(false);
    m_volValueLabel->setText(QString(vol > 0.0f ? "+%1 dB" : "%1 dB").arg(vol, 0, 'f', 1));
    m_volValueLabel->setStyleSheet(vol > 0.0f ? "color: #ff3b30; font-weight: bold;"
                                              : "color: #8e8e93; font-weight: bold;");
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
    if (!layout)
        return;

    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    if (!m_dsp || !m_dsp->pipelineStore())
        return;

    // Resampler chip button
    bool resampEnabled = m_settings ? m_settings->resamplerEnabled : false;
    auto resampChip = new QPushButton("🔄 Resampler", m_pipelineMiniCard);
    resampChip->setCheckable(true);
    resampChip->setChecked(resampEnabled);
    auto updateResampStyle = [resampChip](bool chk) {
        if (chk)
            resampChip->setStyleSheet("background-color: #007aff; color: white; font-size: 10px; border-radius: 4px; "
                                      "padding: 2px 6px; font-weight: bold;");
        else
            resampChip->setStyleSheet(
                "background-color: #3a3a3c; color: #8e8e93; font-size: 10px; border-radius: 4px; padding: 2px 6px;");
    };
    updateResampStyle(resampEnabled);
    connect(resampChip, &QPushButton::clicked, [this, resampChip, updateResampStyle]() {
        if (m_settings) {
            bool enabled = !m_settings->resamplerEnabled;
            m_settings->resamplerEnabled = enabled;
            m_settings->savePreferences();
            resampChip->setChecked(enabled);
            updateResampStyle(enabled);
            m_dsp->applyConfig();
        }
    });
    layout->addWidget(resampChip);

    // Stage chips
    for (const auto& stage : m_dsp->pipelineStore()->stages) {
        std::string icon = stageTypeToIcon(stage.type);
        QString stageTitle = QString("%1 %2").arg(QString::fromStdString(icon)).arg(QString::fromStdString(stage.name));
        auto chip = new QPushButton(stageTitle, m_pipelineMiniCard);
        chip->setCheckable(true);
        chip->setChecked(stage.isEnabled);

        auto updateStyle = [chip](bool chk) {
            if (chk)
                chip->setStyleSheet("background-color: #007aff; color: white; font-size: 10px; border-radius: 4px; "
                                    "padding: 2px 6px; font-weight: bold;");
            else
                chip->setStyleSheet("background-color: #3a3a3c; color: #8e8e93; font-size: 10px; border-radius: 4px; "
                                    "padding: 2px 6px;");
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
                    m_dsp->pipelineStore()->save();
                    emit m_dsp->pipelineStore()->pipelineChanged();
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
        if (m_dsp->status == ProcessingState::Running)
            m_dsp->stopEngine();
        else
            m_dsp->startEngine();
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
        m_volValueLabel->setText(QString(db > 0.0f ? "+%1 dB" : "%1 dB").arg(db, 0, 'f', 1));
        m_volValueLabel->setStyleSheet(db > 0.0f ? "color: #ff3b30; font-weight: bold;"
                                                 : "color: #8e8e93; font-weight: bold;");
    });

    topBar->addWidget(m_volSlider);
    topBar->addWidget(m_volValueLabel);

    // 6 Mode Icon Buttons matching SwiftUI icons
    auto pipeBtn = new QPushButton("🔄", this);
    pipeBtn->setToolTip("Pipeline Overview");
    pipeBtn->setFixedSize(24, 22);
    connect(pipeBtn, &QPushButton::clicked, [this]() {
        buildMiniPipelineUi();
        m_viewStack->setCurrentIndex(0);
    });
    topBar->addWidget(pipeBtn);

    auto specBtn = new QPushButton("📈", this);
    specBtn->setToolTip("Spectrum Analyzer");
    specBtn->setFixedSize(24, 22);
    connect(specBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(1); });
    topBar->addWidget(specBtn);

    auto vuBtn = new QPushButton("🎛️", this);
    vuBtn->setToolTip("Analog VU Meter");
    vuBtn->setFixedSize(24, 22);
    connect(vuBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(3); });
    topBar->addWidget(vuBtn);

    auto mtrBtn = new QPushButton("📊", this);
    mtrBtn->setToolTip("Level Meters");
    mtrBtn->setFixedSize(24, 22);
    connect(mtrBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(2); });
    topBar->addWidget(mtrBtn);

    auto sgBtn = new QPushButton("🌌", this);
    sgBtn->setToolTip("Spectroscope Waterfall");
    sgBtn->setFixedSize(24, 22);
    connect(sgBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(4); });
    topBar->addWidget(sgBtn);

    auto vecBtn = new QPushButton("🎯", this);
    vecBtn->setToolTip("Vector Scope");
    vecBtn->setFixedSize(24, 22);
    connect(vecBtn, &QPushButton::clicked, [this]() { m_viewStack->setCurrentIndex(5); });
    topBar->addWidget(vecBtn);

    auto closeBtn = new QPushButton("✕", this);
    closeBtn->setFixedSize(18, 18);
    connect(closeBtn, &QPushButton::clicked, this, &MiniPlayerView::closeAndRestoreMain);
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

    // Mode 2: Level Meters
    m_metersView = new LevelMeterView(this);
    if (m_monitoring)
        m_metersView->setLevelState(&m_monitoring->levelState);
    m_viewStack->addWidget(m_metersView);

    // Mode 3: Analog VU
    m_analogVUView = new AnalogVUMeterView(this);
    if (m_monitoring)
        m_analogVUView->setLevelState(&m_monitoring->levelState);
    m_viewStack->addWidget(m_analogVUView);

    // Mode 4: Spectrogram
    m_spectrogramView = new SpectrogramView(m_monitoring ? m_monitoring->spectrogramEngine() : nullptr, this);
    m_viewStack->addWidget(m_spectrogramView);

    // Mode 5: Vector Scope
    m_vectorScopeView = new VectorScopeView(m_monitoring ? m_monitoring->vectorScopeEngine() : nullptr, this);
    m_viewStack->addWidget(m_vectorScopeView);

    mainLayout->addWidget(m_viewStack);
    updateEngineStatus(m_dsp->status);
}

void MiniPlayerView::closeAndRestoreMain() {
    hide();
    emit requestRestoreMainWindow();
}

void MiniPlayerView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        closeAndRestoreMain();
        event->accept();
    }
}

void MiniPlayerView::keyPressEvent(QKeyEvent* event) {
    bool hasCmdOrCtrl = (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));

    if (event->key() == Qt::Key_Escape || (hasCmdOrCtrl && event->key() == Qt::Key_W) ||
        (hasCmdOrCtrl && event->key() == Qt::Key_M)) {
        closeAndRestoreMain();
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Space) {
        if (m_dsp) {
            if (m_dsp->status == ProcessingState::Running)
                m_dsp->stopEngine();
            else
                m_dsp->startEngine();
        }
        event->accept();
        return;
    } else if (event->key() == Qt::Key_M && !hasCmdOrCtrl) {
        if (m_settings && m_dsp) {
            Fader f = currentFader();
            bool muted = m_settings->getMuted(f);
            m_dsp->setFaderMute(f, !muted);
            if (m_muteBtn) {
                m_muteBtn->setText(!muted ? "🔇" : "🔊");
            }
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void MiniPlayerView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void MiniPlayerView::refreshMeters() {
    if (!m_monitoring)
        return;
    const auto& st = m_monitoring->levelState;
    if (m_metersView) {
        m_metersView->setLevels(st.playbackRms, st.playbackPeak, "Playback");
    }
    if (m_analogVUView) {
        float left = !st.playbackPeak.empty() ? st.playbackPeak[0] : -60.0f;
        float right = st.playbackPeak.size() > 1 ? st.playbackPeak[1] : left;
        m_analogVUView->setLevelDB(left, right);
    }
    if (m_spectrumView && m_monitoring->spectrumEngine()) {
        m_spectrumView->setSpectrum(m_monitoring->spectrumEngine()->data);
    }
    if (m_spectrogramView && m_monitoring->spectrogramEngine()) {
        m_spectrogramView->setHistory(m_monitoring->spectrogramEngine()->history,
                                      m_monitoring->spectrogramEngine()->show3D);
    }
    if (m_vectorScopeView && m_monitoring->vectorScopeEngine()) {
        m_vectorScopeView->setSamples(m_monitoring->vectorScopeEngine()->samples,
                                      m_monitoring->vectorScopeEngine()->showParticles);
    }
}
