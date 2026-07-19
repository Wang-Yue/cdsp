#include "ui/MiniPlayerView.h"

#include "ui/StyleTheme.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWindow>

#ifdef Q_OS_MAC
#include <objc/message.h>
#include <objc/runtime.h>

template <typename Target, typename... Args>
static void safeSendObjcMsg(Target target, const char* selName, Args... args) {
    if (!target)
        return;
    SEL sel = sel_registerName(selName);
    SEL respondsSel = sel_registerName("respondsToSelector:");
    auto respondsFunc = reinterpret_cast<bool (*)(Target, SEL, SEL)>(objc_msgSend);
    if (respondsFunc(target, respondsSel, sel)) {
        auto sendFunc = reinterpret_cast<void (*)(Target, SEL, Args...)>(objc_msgSend);
        sendFunc(target, sel, args...);
    }
}

static void setMacFloatingPanelProperties(QWidget* widget) {
    if (auto window = widget->windowHandle()) {
        void* view = reinterpret_cast<void*>(window->winId());
        if (view) {
            void* nsWindow = reinterpret_cast<void* (*)(void*, SEL)>(objc_msgSend)(view, sel_registerName("window"));
            if (nsWindow) {
                unsigned long behavior = (1UL << 0) | (1UL << 8) | (1UL << 6);
                safeSendObjcMsg(nsWindow, "setCollectionBehavior:", behavior);
                safeSendObjcMsg(nsWindow, "setLevel:", 1000L);
                safeSendObjcMsg(nsWindow, "setMovableByWindowBackground:", true);
                safeSendObjcMsg(nsWindow, "setHidesOnDeactivate:", false);
                safeSendObjcMsg(nsWindow, "setBecomesKeyOnlyIfNeeded:", true);
                safeSendObjcMsg(nsWindow, "setTitlebarAppearsTransparent:", true);
                safeSendObjcMsg(nsWindow, "setTitleVisibility:", 1L);
                safeSendObjcMsg(nsWindow, "setHasShadow:", true);
                safeSendObjcMsg(nsWindow, "setFloatingPanel:", true);
            }
        }
    }
}
#endif

MiniPlayerView::MiniPlayerView(std::shared_ptr<DSPEngineController> dsp, std::shared_ptr<AudioSettings> settings,
                               std::shared_ptr<MonitoringController> monitoring, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_dsp(dsp),
      m_settings(settings), m_monitoring(monitoring) {

    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("QWidget#MiniPlayerViewWindow { background-color: rgba(0, 0, 0, 0.45); border-radius: 12px; "
                  "border: none; }");
    setObjectName("MiniPlayerViewWindow");
    setMinimumSize(200, 80);
    setMaximumSize(1000, 300);
    resize(320, 90);
    setFocusPolicy(Qt::StrongFocus);

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
    QSettings settings;
    if (settings.contains("MiniPlayer/geometry")) {
        restoreGeometry(settings.value("MiniPlayer/geometry").toByteArray());
    } else {
        if (auto screen = QGuiApplication::primaryScreen()) {
            QRect screenFrame = screen->availableGeometry();
            int x = screenFrame.x() + screenFrame.width() - 330;
            int y = screenFrame.y() + screenFrame.height() - 100;
            move(x, y);
        }
    }
    int savedMode = 1;
    if (settings.contains("mini_player_mode")) {
        savedMode = settings.value("mini_player_mode", 1).toInt();
    } else {
        savedMode = settings.value("MiniPlayer/mode", 1).toInt();
    }
    if (m_viewStack && savedMode >= 0 && savedMode < m_viewStack->count()) {
        m_viewStack->setCurrentIndex(savedMode);
        updateModeButtonStyles(savedMode);
    }
}

Fader MiniPlayerView::currentFader() const {
    return Fader::Main;
}

void MiniPlayerView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QWidget* child = childAt(event->position().toPoint());
        if (!child || (!qobject_cast<QAbstractButton*>(child) && !qobject_cast<QAbstractSlider*>(child))) {
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

void MiniPlayerView::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    if (m_headerOpacityEffect) {
        auto anim = new QPropertyAnimation(m_headerOpacityEffect, "opacity", this);
        anim->setDuration(200);
        anim->setEndValue(1.0);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void MiniPlayerView::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    if (m_headerOpacityEffect) {
        auto anim = new QPropertyAnimation(m_headerOpacityEffect, "opacity", this);
        anim->setDuration(200);
        anim->setEndValue(0.3);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
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
    m_volValueLabel->setText(QString::asprintf("%+.0f", vol));
    m_volValueLabel->setStyleSheet(vol > 0.0f
                                       ? "color: #ff3b30; font-family: monospace; font-size: 9px;"
                                       : "color: rgba(255, 255, 255, 0.7); font-family: monospace; font-size: 9px;");
    m_muteBtn->setText(muted ? "🔇" : "🔊");
    m_muteBtn->setStyleSheet(muted ? "QPushButton { background: transparent; color: #ff3b30; border: none; font-size: "
                                     "10px; padding: 0px; margin: 0px; }"
                                   : "QPushButton { background: transparent; color: rgba(255, 255, 255, 0.5); border: "
                                     "none; font-size: 10px; padding: 0px; margin: 0px; } "
                                     "QPushButton:hover { color: rgba(255, 255, 255, 0.9); }");
}

void MiniPlayerView::updateEngineStatus(ProcessingState state) {
    if (!m_playStopBtn)
        return;
    if (state == ProcessingState::Running) {
        m_playStopBtn->setText("⏹");
    } else {
        m_playStopBtn->setText("▶");
    }
    m_playStopBtn->setStyleSheet("QPushButton { background: transparent; color: rgba(255, 255, 255, 0.5); border: "
                                 "none; font-size: 10px; padding: 0px; margin: 0px; } "
                                 "QPushButton:hover { color: rgba(255, 255, 255, 0.9); }");
    buildMiniPipelineUi();
}

void MiniPlayerView::buildMiniPipelineUi() {
    if (!m_pipelineMiniCard)
        return;
    auto layout = qobject_cast<QHBoxLayout*>(m_pipelineMiniCard->layout());
    if (!layout)
        return;

    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    if (!m_dsp)
        return;

    bool isRunning = (m_dsp->status == ProcessingState::Running);

    auto makePillStyle = [](bool active, const QString& activeBgOverride = "") {
        if (active) {
            QString bg = activeBgOverride.isEmpty() ? "#007aff" : activeBgOverride;
            return QString("background-color: %1; color: #ffffff; font-size: 9px; border-radius: 9px; padding: 2px "
                           "7px; font-weight: bold; border: none;")
                .arg(bg);
        } else {
            return QString("background-color: rgba(255, 255, 255, 0.15); color: rgba(255, 255, 255, 0.6); font-size: "
                           "9px; border-radius: 9px; padding: 2px 7px; border: none;");
        }
    };

    auto addChevron = [layout, card = m_pipelineMiniCard]() {
        auto label = new QLabel("›", card);
        label->setStyleSheet("color: rgba(255, 255, 255, 0.3); font-size: 11px; font-weight: bold; border: none; "
                             "background: transparent;");
        layout->addWidget(label);
    };

    // 1. Input Chip
    QString inDev = "Input";
    if (m_dsp->devices()) {
        auto optName = m_dsp->devices()->captureConfig.deviceName();
        if (optName.has_value() && !optName->empty()) {
            inDev = QString::fromStdString(*optName);
        }
    }
    auto inChip = new QLabel(QString("🎤 %1").arg(inDev), m_pipelineMiniCard);
    inChip->setStyleSheet(makePillStyle(isRunning, "#007aff"));
    layout->addWidget(inChip);

    addChevron();

    // 2. Resampler Chip
    bool resampEnabled = m_settings ? m_settings->resamplerEnabled : false;
    auto resampChip = new QPushButton("🔄 Resampler", m_pipelineMiniCard);
    resampChip->setCheckable(true);
    resampChip->setChecked(resampEnabled);
    resampChip->setStyleSheet(makePillStyle(resampEnabled));
    connect(resampChip, &QPushButton::clicked, [this, resampChip, makePillStyle]() {
        if (m_settings) {
            bool enabled = !m_settings->resamplerEnabled;
            m_settings->resamplerEnabled = enabled;
            m_settings->savePreferences();
            resampChip->setChecked(enabled);
            resampChip->setStyleSheet(makePillStyle(enabled));
            m_dsp->applyConfig();
        }
    });
    layout->addWidget(resampChip);

    // 3. Stage Chips
    if (m_dsp->pipelineStore()) {
        for (const auto& stage : m_dsp->pipelineStore()->stages) {
            addChevron();
            std::string icon = stageTypeToIcon(stage.type);
            QString stageTitle =
                QString("%1 %2").arg(QString::fromStdString(icon)).arg(QString::fromStdString(stage.name));
            auto chip = new QPushButton(stageTitle, m_pipelineMiniCard);
            chip->setCheckable(true);
            chip->setChecked(stage.isEnabled);
            chip->setStyleSheet(makePillStyle(stage.isEnabled));

            QUuid id = stage.id;
            connect(chip, &QPushButton::clicked, [this, id, chip, makePillStyle]() {
                if (m_dsp && m_dsp->pipelineStore()) {
                    auto& stages = m_dsp->pipelineStore()->stages;
                    for (auto& st : stages) {
                        if (st.id == id) {
                            st.isEnabled = !st.isEnabled;
                            chip->setChecked(st.isEnabled);
                            chip->setStyleSheet(makePillStyle(st.isEnabled));
                            m_dsp->pipelineStore()->save();
                            emit m_dsp->pipelineStore()->pipelineChanged();
                            break;
                        }
                    }
                }
            });
            layout->addWidget(chip);
        }
    }

    addChevron();

    // 4. Output Chip
    QString outDev = "Output";
    if (m_dsp->devices()) {
        auto optName = m_dsp->devices()->playbackConfig.deviceName();
        if (optName.has_value() && !optName->empty()) {
            outDev = QString::fromStdString(*optName);
        }
    }
    auto outChip = new QLabel(QString("🔊 %1").arg(outDev), m_pipelineMiniCard);
    outChip->setStyleSheet(makePillStyle(isRunning, "#34c759"));
    layout->addWidget(outChip);

    layout->addStretch();
}

void MiniPlayerView::updateModeButtonStyles(int activeIndex) {
    for (int i = 0; i < static_cast<int>(m_modeBtns.size()); ++i) {
        if (i == activeIndex) {
            m_modeBtns[i]->setStyleSheet(
                "QPushButton { background: transparent; color: #ffffff; border: none; font-size: 10px; "
                "padding: 0px; margin: 0px; text-align: center; }");
        } else {
            m_modeBtns[i]->setStyleSheet(
                "QPushButton { background: transparent; color: rgba(255, 255, 255, 0.4); border: none; font-size: "
                "10px; padding: 0px; margin: 0px; text-align: center; } "
                "QPushButton:hover { color: rgba(255, 255, 255, 0.8); }");
        }
    }
    QSettings settings;
    settings.setValue("mini_player_mode", activeIndex);
    settings.setValue("MiniPlayer/mode", activeIndex);
    refreshMeters();
}

void MiniPlayerView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto topBarWidget = new QWidget(this);
    auto topBar = new QHBoxLayout(topBarWidget);
    topBar->setContentsMargins(8, 4, 8, 4);
    topBar->setSpacing(6);

    m_headerOpacityEffect = new QGraphicsOpacityEffect(topBarWidget);
    topBarWidget->setGraphicsEffect(m_headerOpacityEffect);
    m_headerOpacityEffect->setOpacity(0.3);

    // Play / Stop button
    m_playStopBtn = new QPushButton("▶", topBarWidget);
    m_playStopBtn->setFixedSize(18, 18);
    m_playStopBtn->setStyleSheet("QPushButton { background: transparent; color: rgba(255, 255, 255, 0.5); border: "
                                 "none; font-size: 10px; padding: 0px; margin: 0px; } "
                                 "QPushButton:hover { color: rgba(255, 255, 255, 0.9); }");
    connect(m_playStopBtn, &QPushButton::clicked, [this]() {
        if (m_dsp->status == ProcessingState::Running)
            m_dsp->stopEngine();
        else
            m_dsp->startEngine();
    });
    topBar->addWidget(m_playStopBtn);

    // Volume Control Row container
    auto volWidget = new QWidget(topBarWidget);
    auto volLayout = new QHBoxLayout(volWidget);
    volLayout->setContentsMargins(8, 2, 8, 2);
    volLayout->setSpacing(6);

    m_muteBtn = new QPushButton("🔊", volWidget);
    m_muteBtn->setFixedSize(18, 18);
    m_muteBtn->setStyleSheet("QPushButton { background: transparent; color: rgba(255, 255, 255, 0.5); border: none; "
                             "font-size: 10px; padding: 0px; margin: 0px; } "
                             "QPushButton:hover { color: rgba(255, 255, 255, 0.9); }");
    connect(m_muteBtn, &QPushButton::clicked, [this]() {
        Fader f = currentFader();
        bool muted = m_settings->getMuted(f);
        m_dsp->setFaderMute(f, !muted);
        m_muteBtn->setText(!muted ? "🔇" : "🔊");
    });
    volLayout->addWidget(m_muteBtn);

    m_volSlider = new QSlider(Qt::Horizontal, volWidget);
    m_volSlider->setRange(-120, 40); // -60 dB to +20 dB
    float currentVol = m_settings ? m_settings->getVolume(Fader::Main) : 0.0f;
    m_volSlider->setValue(static_cast<int>(currentVol * 2.0f));
    m_volSlider->setStyleSheet(
        "QSlider::groove:horizontal { height: 3px; background: rgba(255, 255, 255, 0.2); border-radius: 1.5px; } "
        "QSlider::sub-page:horizontal { background: #007aff; border-radius: 1.5px; } "
        "QSlider::handle:horizontal { background: #ffffff; width: 10px; height: 10px; margin: -3.5px 0; border-radius: "
        "5px; }");

    m_volValueLabel = new QLabel(QString::asprintf("%+.0f", currentVol), volWidget);
    m_volValueLabel->setFont(QFont("monospace", 9));
    m_volValueLabel->setFixedWidth(25);
    m_volValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volValueLabel->setStyleSheet(currentVol > 0.0f
                                       ? "color: #ff3b30; font-family: monospace; font-size: 9px;"
                                       : "color: rgba(255, 255, 255, 0.7); font-family: monospace; font-size: 9px;");

    connect(m_volSlider, &QSlider::valueChanged, [this](int val) {
        Fader f = currentFader();
        float db = val / 2.0f;
        m_dsp->setFaderVolume(f, db);
        m_volValueLabel->setText(QString::asprintf("%+.0f", db));
        m_volValueLabel->setStyleSheet(
            db > 0.0f ? "color: #ff3b30; font-family: monospace; font-size: 9px;"
                      : "color: rgba(255, 255, 255, 0.7); font-family: monospace; font-size: 9px;");
    });

    volLayout->addWidget(m_volSlider);
    volLayout->addWidget(m_volValueLabel);
    topBar->addWidget(volWidget);

    // 6 Mode Icon Buttons matching SwiftUI icons
    auto pipeBtn = new QPushButton("☍", topBarWidget);
    auto specBtn = new QPushButton("〰", topBarWidget);
    auto mtrBtn = new QPushButton("📊", topBarWidget);
    auto vuBtn = new QPushButton("⏱", topBarWidget);
    auto sgBtn = new QPushButton("▦", topBarWidget);
    auto vecBtn = new QPushButton("⚡", topBarWidget);

    m_modeBtns = {pipeBtn, specBtn, mtrBtn, vuBtn, sgBtn, vecBtn};

    pipeBtn->setToolTip("Pipeline Overview");
    pipeBtn->setFixedSize(18, 18);
    connect(pipeBtn, &QPushButton::clicked, [this]() {
        buildMiniPipelineUi();
        m_viewStack->setCurrentIndex(0);
        updateModeButtonStyles(0);
    });
    topBar->addWidget(pipeBtn);

    specBtn->setToolTip("Spectrum Analyzer");
    specBtn->setFixedSize(18, 18);
    connect(specBtn, &QPushButton::clicked, [this]() {
        m_viewStack->setCurrentIndex(1);
        updateModeButtonStyles(1);
    });
    topBar->addWidget(specBtn);

    mtrBtn->setToolTip("Level Meters");
    mtrBtn->setFixedSize(18, 18);
    connect(mtrBtn, &QPushButton::clicked, [this]() {
        m_viewStack->setCurrentIndex(2);
        updateModeButtonStyles(2);
    });
    topBar->addWidget(mtrBtn);

    vuBtn->setToolTip("Analog VU Meter");
    vuBtn->setFixedSize(18, 18);
    connect(vuBtn, &QPushButton::clicked, [this]() {
        m_viewStack->setCurrentIndex(3);
        updateModeButtonStyles(3);
    });
    topBar->addWidget(vuBtn);

    sgBtn->setToolTip("Spectroscope Waterfall");
    sgBtn->setFixedSize(18, 18);
    connect(sgBtn, &QPushButton::clicked, [this]() {
        m_viewStack->setCurrentIndex(4);
        updateModeButtonStyles(4);
    });
    topBar->addWidget(sgBtn);

    vecBtn->setToolTip("Vector Scope");
    vecBtn->setFixedSize(18, 18);
    connect(vecBtn, &QPushButton::clicked, [this]() {
        m_viewStack->setCurrentIndex(5);
        updateModeButtonStyles(5);
    });
    topBar->addWidget(vecBtn);

    updateModeButtonStyles(1); // Default to Spectrum (mode 1) matching SwiftUI default

    mainLayout->addWidget(topBarWidget);

    m_viewStack = new QStackedWidget(this);
    m_viewStack->setContentsMargins(8, 0, 8, 8);
    m_viewStack->setStyleSheet("QStackedWidget { background: transparent; }");

    // Mode 0: Mini Pipeline Chips in a QScrollArea
    auto pipeScroll = new QScrollArea(this);
    pipeScroll->setWidgetResizable(true);
    pipeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pipeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pipeScroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    m_pipelineMiniCard = new QWidget(pipeScroll);
    m_pipelineMiniCard->setStyleSheet("QWidget { background: transparent; }");
    auto pipeLayout = new QHBoxLayout(m_pipelineMiniCard);
    pipeLayout->setContentsMargins(0, 0, 0, 0);
    pipeLayout->setSpacing(4);
    buildMiniPipelineUi();
    pipeScroll->setWidget(m_pipelineMiniCard);
    m_viewStack->addWidget(pipeScroll);

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

    topBarWidget->installEventFilter(this);
    m_viewStack->installEventFilter(this);
    if (pipeScroll)
        pipeScroll->installEventFilter(this);
    if (m_pipelineMiniCard)
        m_pipelineMiniCard->installEventFilter(this);
    if (m_spectrumView)
        m_spectrumView->installEventFilter(this);
    if (m_metersView)
        m_metersView->installEventFilter(this);
    if (m_analogVUView)
        m_analogVUView->installEventFilter(this);
    if (m_spectrogramView)
        m_spectrogramView->installEventFilter(this);
    if (m_vectorScopeView)
        m_vectorScopeView->installEventFilter(this);

    onFaderChanged(0);
}

void MiniPlayerView::closeAndRestoreMain() {
    QSettings settings;
    settings.setValue("MiniPlayer/geometry", saveGeometry());
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
    p.setRenderHint(QPainter::Antialiasing);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

bool MiniPlayerView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto mouseEv = static_cast<QMouseEvent*>(event);
        if (mouseEv->button() == Qt::LeftButton) {
            QWidget* child = qobject_cast<QWidget*>(watched);
            if (child && !qobject_cast<QAbstractButton*>(child) && !qobject_cast<QAbstractSlider*>(child)) {
                closeAndRestoreMain();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MiniPlayerView::refreshMeters() {
    if (!m_monitoring || !m_viewStack)
        return;

    int mode = m_viewStack->currentIndex();
    const auto& st = m_monitoring->levelState;

    switch (mode) {
    case 1: // Spectrum
        if (m_spectrumView && m_monitoring->spectrumEngine()) {
            m_spectrumView->setSpectrum(m_monitoring->spectrumEngine()->data);
        }
        break;
    case 2: // Level Meters
        if (m_metersView) {
            m_metersView->setLevels(st.playbackRms, st.playbackPeak, "");
        }
        break;
    case 3: // Analog VU
        if (m_analogVUView) {
            float left = !st.playbackPeak.empty() ? st.playbackPeak[0] : -60.0f;
            float right = st.playbackPeak.size() > 1 ? st.playbackPeak[1] : left;
            m_analogVUView->setLevelDB(left, right);
        }
        break;
    case 4: // Spectrogram
        if (m_spectrogramView && m_monitoring->spectrogramEngine()) {
            m_spectrogramView->setHistory(m_monitoring->spectrogramEngine()->history,
                                          m_monitoring->spectrogramEngine()->show3D);
        }
        break;
    case 5: // Vector Scope
        if (m_vectorScopeView && m_monitoring->vectorScopeEngine()) {
            m_vectorScopeView->setSamples(m_monitoring->vectorScopeEngine()->samples,
                                          m_monitoring->vectorScopeEngine()->showParticles);
        }
        break;
    default:
        break;
    }
}
