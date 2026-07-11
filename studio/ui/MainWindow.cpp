#include "ui/MainWindow.h"
#include "ui/StyleTheme.h"
#include "ui/DashboardView.h"
#include "ui/DevicePickerView.h"
#include "ui/EQPresetDetailView.h"
#include "ui/StageDetailView.h"
#include "ui/ResamplerDetailView.h"
#include "ui/GeneralSettingsView.h"
#include "ui/ConvolutionPresetDetailView.h"
#include "ui/ConsoleLogsView.h"
#include "ui/AutoEqPickerDlg.h"
#include "ui/OratoryPresetPickerDlg.h"
#include "ui/ConvolutionImportDlg.h"
#include "ui/RoomCorrectionDlg.h"
#include "ui/LevelMeterView.h"
#include "ui/SpectrumView.h"
#include "ui/SpectrogramView.h"
#include "ui/VectorScopeView.h"
#include "ui/AnalogVUMeterView.h"
#include "ui/VisualizerDetailViews.h"

#include <QApplication>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QMenu>
#include <QAction>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QCursor>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_engine = std::make_shared<CDSPEngine>();
    m_settings = std::make_shared<AudioSettings>();
    m_devices = std::make_shared<AudioDeviceManager>(m_engine, m_settings);
    m_pipeline = std::make_shared<PipelineStore>();
    m_dspController = std::make_shared<DSPEngineController>(m_engine, m_devices, m_settings, m_pipeline);

    m_spectrumEngine = std::make_shared<SpectrumEngine>();
    m_spectrogramEngine = std::make_shared<SpectrogramEngine>();
    m_vectorScopeEngine = std::make_shared<VectorScopeEngine>();
    m_monitoring = std::make_shared<MonitoringController>(
        m_engine, m_dspController, m_spectrumEngine, m_spectrogramEngine, m_vectorScopeEngine
    );

    m_miniPlayer = std::make_unique<MiniPlayerView>(m_dspController, m_settings, m_monitoring);

    resize(1280, 800);
    setWindowTitle("CamillaDSP Monitor - Qt Edition");

    setupUi();
    setupStatusBar();
    setupTrayIcon();
    setupShortcuts();
    updateTheme();

    connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, &MainWindow::onPipelineChanged);
    connect(m_dspController.get(), &DSPEngineController::statusChanged, this, &MainWindow::onEngineStatusChanged);
    connect(m_settings.get(), &AudioSettings::settingsChanged, this, &MainWindow::updateTheme);

    m_monitoring->start();
}

void MainWindow::updateTheme() {
    setStyleSheet(m_settings->darkMode ? StyleTheme::darkStylesheet() : StyleTheme::lightStylesheet());
}

void MainWindow::setupUi() {
    setupToolbar();

    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);

    m_sidebarTree = new QTreeWidget(m_splitter);
    m_sidebarTree->setHeaderHidden(true);
    m_sidebarTree->setMinimumWidth(240);
    m_sidebarTree->setMaximumWidth(320);
    m_sidebarTree->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_sidebarTree, &QTreeWidget::itemClicked, this, &MainWindow::onSidebarItemClicked);
    connect(m_sidebarTree, &QTreeWidget::customContextMenuRequested, [this](const QPoint& pos) {
        auto item = m_sidebarTree->itemAt(pos);
        if (!item) return;
        QString tag = item->data(0, Qt::UserRole).toString();

        if (tag.startsWith("stage_")) {
            int idx = tag.mid(6).toInt();
            QMenu menu(this);
            auto moveUp = menu.addAction("Move Up");
            moveUp->setEnabled(idx > 0);
            connect(moveUp, &QAction::triggered, [this, idx]() {
                m_pipeline->moveStage(idx, idx - 1);
            });

            auto moveDown = menu.addAction("Move Down");
            moveDown->setEnabled(idx < static_cast<int>(m_pipeline->stages.size()) - 1);
            connect(moveDown, &QAction::triggered, [this, idx]() {
                m_pipeline->moveStage(idx, idx + 1);
            });

            menu.addSeparator();
            auto del = menu.addAction("Delete Stage");
            connect(del, &QAction::triggered, [this, idx]() {
                if (idx < static_cast<int>(m_pipeline->stages.size())) {
                    m_pipeline->deleteStage(m_pipeline->stages[idx].id);
                }
            });
            menu.exec(QCursor::pos());
        }
    });

    m_centralStack = new QStackedWidget(m_splitter);

    m_splitter->addWidget(m_sidebarTree);
    m_splitter->addWidget(m_centralStack);
    m_splitter->setSizes({260, 1020});

    setupSidebar();
}

void MainWindow::setupStatusBar() {
    auto bar = statusBar();
    m_statusStateLabel = new QLabel("State: Inactive", this);
    m_statusBufferLabel = new QLabel("Buffer: 1024", this);
    m_statusActivePresetLabel = new QLabel("Preset: Default", this);
    m_statusMuteLabel = new QLabel("Unmuted", this);

    m_statusStateLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusBufferLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusActivePresetLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #34c759; font-weight: bold;");

    bar->addWidget(m_statusStateLabel);
    bar->addWidget(m_statusBufferLabel);
    bar->addWidget(m_statusActivePresetLabel);
    bar->addPermanentWidget(m_statusMuteLabel);
}

void MainWindow::setupTrayIcon() {
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QIcon::fromTheme("audio-card", QIcon(":/icons/app.png")));

    m_trayMenu = new QMenu(this);

    auto showAct = m_trayMenu->addAction("Show Main Window");
    connect(showAct, &QAction::triggered, this, &MainWindow::showNormal);

    auto miniPlayerAct = m_trayMenu->addAction("Toggle MiniPlayer");
    connect(miniPlayerAct, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);

    m_trayMenu->addSeparator();

    auto muteAct = m_trayMenu->addAction("Toggle Mute");
    connect(muteAct, &QAction::triggered, this, &MainWindow::toggleMute);

    m_trayMenu->addSeparator();

    auto quitAct = m_trayMenu->addAction("Quit CamillaDSP Monitor");
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->show();
}

void MainWindow::setupShortcuts() {
    auto actDash = new QAction(this);
    actDash->setShortcut(QKeySequence("Ctrl+1"));
    connect(actDash, &QAction::triggered, [this]() { handleNavigationTag("dashboard"); });
    addAction(actDash);

    auto actDev = new QAction(this);
    actDev->setShortcut(QKeySequence("Ctrl+2"));
    connect(actDev, &QAction::triggered, [this]() { handleNavigationTag("devices"); });
    addAction(actDev);

    auto actLevels = new QAction(this);
    actLevels->setShortcut(QKeySequence("Ctrl+3"));
    connect(actLevels, &QAction::triggered, [this]() { handleNavigationTag("levels"); });
    addAction(actLevels);

    auto actSpec = new QAction(this);
    actSpec->setShortcut(QKeySequence("Ctrl+4"));
    connect(actSpec, &QAction::triggered, [this]() { handleNavigationTag("spectrum"); });
    addAction(actSpec);

    auto actSettings = new QAction(this);
    actSettings->setShortcut(QKeySequence("Ctrl+5"));
    connect(actSettings, &QAction::triggered, [this]() { handleNavigationTag("general_settings"); });
    addAction(actSettings);

    auto actMini = new QAction(this);
    actMini->setShortcut(QKeySequence("Ctrl+M"));
    connect(actMini, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);
    addAction(actMini);

    auto actMute = new QAction(this);
    actMute->setShortcut(QKeySequence("Space"));
    connect(actMute, &QAction::triggered, this, &MainWindow::toggleMute);
    addAction(actMute);
}

void MainWindow::toggleMute() {
    bool currentMute = m_settings->getMuted(Fader::Main);
    m_dspController->setFaderMute(Fader::Main, !currentMute);
    if (!currentMute) {
        m_statusMuteLabel->setText("MUTED");
        m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #ff3b30; font-weight: bold;");
    } else {
        m_statusMuteLabel->setText("Unmuted");
        m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #34c759; font-weight: bold;");
    }
}

void MainWindow::setupToolbar() {
    auto toolBar = addToolBar("Main Controls");
    toolBar->setMovable(false);

    m_startStopBtn = new QPushButton("Start Engine", this);
    m_startStopBtn->setStyleSheet("background-color: #34c759; color: white;");
    connect(m_startStopBtn, &QPushButton::clicked, [this]() {
        if (m_dspController->status == ProcessingState::Running) {
            m_dspController->stopEngine();
        } else {
            m_dspController->startEngine();
        }
    });
    toolBar->addWidget(m_startStopBtn);

    toolBar->addSeparator();

    m_sampleRateBadge = new QLabel("48000 Hz", this);
    m_sampleRateBadge->setFont(QFont("monospace", 12, QFont::Bold));
    m_sampleRateBadge->setStyleSheet("color: #6c6c70; padding: 0 12px;");
    toolBar->addWidget(m_sampleRateBadge);

    toolBar->addSeparator();

    auto volLabel = new QLabel(" Volume:", this);
    toolBar->addWidget(volLabel);

    m_headerVolumeSlider = new QSlider(Qt::Horizontal, this);
    m_headerVolumeSlider->setRange(-60, 10);
    m_headerVolumeSlider->setValue(0);
    m_headerVolumeSlider->setFixedWidth(140);
    connect(m_headerVolumeSlider, &QSlider::valueChanged, [this](int val) {
        m_dspController->setFaderVolume(Fader::Main, static_cast<float>(val));
    });
    toolBar->addWidget(m_headerVolumeSlider);

    toolBar->addSeparator();

    auto miniPlayerBtn = new QPushButton("Floating MiniPlayer", this);
    connect(miniPlayerBtn, &QPushButton::clicked, this, &MainWindow::toggleMiniPlayer);
    toolBar->addWidget(miniPlayerBtn);
}

void MainWindow::toggleMiniPlayer() {
    if (m_miniPlayer->isVisible()) {
        m_miniPlayer->hide();
    } else {
        m_miniPlayer->show();
        m_miniPlayer->raise();
        m_miniPlayer->activateWindow();
    }
}

void MainWindow::setupSidebar() {
    refreshSidebarItems();
    handleNavigationTag("dashboard");
}

void MainWindow::refreshSidebarItems() {
    m_sidebarTree->blockSignals(true);
    m_sidebarTree->clear();

    // 1. Audio Section
    auto audioGroup = new QTreeWidgetItem(m_sidebarTree, {"Audio"});
    audioGroup->setExpanded(true);
    auto devItem = new QTreeWidgetItem(audioGroup, {"Devices"});
    devItem->setData(0, Qt::UserRole, "devices");
    auto dashItem = new QTreeWidgetItem(audioGroup, {"Dashboard"});
    dashItem->setData(0, Qt::UserRole, "dashboard");

    // 2. Monitoring Section
    auto monGroup = new QTreeWidgetItem(m_sidebarTree, {"Monitoring"});
    monGroup->setExpanded(true);
    auto levelsItem = new QTreeWidgetItem(monGroup, {"Level Meters"});
    levelsItem->setData(0, Qt::UserRole, "levels");
    auto specItem = new QTreeWidgetItem(monGroup, {"Spectrum"});
    specItem->setData(0, Qt::UserRole, "spectrum");
    auto spectroItem = new QTreeWidgetItem(monGroup, {"Spectroscope"});
    spectroItem->setData(0, Qt::UserRole, "spectroscope");
    auto vecItem = new QTreeWidgetItem(monGroup, {"Vector Scope"});
    vecItem->setData(0, Qt::UserRole, "vectorscope");
    auto vuItem = new QTreeWidgetItem(monGroup, {"Analog VU"});
    vuItem->setData(0, Qt::UserRole, "analogVU");
    auto logsItem = new QTreeWidgetItem(monGroup, {"Console Logs"});
    logsItem->setData(0, Qt::UserRole, "logs");
    auto settingsItem = new QTreeWidgetItem(monGroup, {"General Settings"});
    settingsItem->setData(0, Qt::UserRole, "general_settings");

    // 3. Pipeline Section
    auto pipeGroup = new QTreeWidgetItem(m_sidebarTree, {"Pipeline"});
    pipeGroup->setExpanded(true);
    auto resItem = new QTreeWidgetItem(pipeGroup, {"Resampler"});
    resItem->setData(0, Qt::UserRole, "resampler");

    for (size_t i = 0; i < m_pipeline->stages.size(); ++i) {
        const auto& stage = m_pipeline->stages[i];
        auto sItem = new QTreeWidgetItem(pipeGroup, {QString::fromStdString(stage.name)});
        sItem->setData(0, Qt::UserRole, QString("stage_%1").arg(i));
    }

    auto addStageItem = new QTreeWidgetItem(pipeGroup, {"+ Add Stage..."});
    addStageItem->setData(0, Qt::UserRole, "add_stage");

    // 4. Convolution Section
    auto convGroup = new QTreeWidgetItem(m_sidebarTree, {"Convolution"});
    convGroup->setExpanded(true);
    for (const auto& conv : m_pipeline->convPresets) {
        auto cItem = new QTreeWidgetItem(convGroup, {QString::fromStdString(conv.name)});
        cItem->setData(0, Qt::UserRole, QString("conv_%1").arg(conv.id.toString()));
    }
    auto impConvItem = new QTreeWidgetItem(convGroup, {"Import IR File(s)..."});
    impConvItem->setData(0, Qt::UserRole, "import_conv");

    auto roomItem = new QTreeWidgetItem(convGroup, {"Room Correction Studio"});
    roomItem->setData(0, Qt::UserRole, "room_correction");

    // 5. EQ Presets Section
    auto eqGroup = new QTreeWidgetItem(m_sidebarTree, {"EQ Presets"});
    eqGroup->setExpanded(true);
    for (const auto& eq : m_pipeline->eqPresets) {
        auto eItem = new QTreeWidgetItem(eqGroup, {QString::fromStdString(eq.name)});
        eItem->setData(0, Qt::UserRole, QString("eq_%1").arg(eq.id.toString()));
    }
    auto addEqItem = new QTreeWidgetItem(eqGroup, {"+ Add Preset"});
    addEqItem->setData(0, Qt::UserRole, "add_eq");
    auto autoEqItem = new QTreeWidgetItem(eqGroup, {"AutoEQ Explorer"});
    autoEqItem->setData(0, Qt::UserRole, "auto_eq");
    auto oratoryItem = new QTreeWidgetItem(eqGroup, {"Oratory1990 Explorer"});
    oratoryItem->setData(0, Qt::UserRole, "oratory_eq");

    m_sidebarTree->blockSignals(false);
}

void MainWindow::showCentralWidget(QWidget* widget) {
    while (m_centralStack->count() > 0) {
        QWidget* w = m_centralStack->widget(0);
        m_centralStack->removeWidget(w);
        w->deleteLater();
    }
    m_centralStack->addWidget(widget);
    m_centralStack->setCurrentWidget(widget);
}

void MainWindow::onSidebarItemClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item) return;

    QString tag = item->data(0, Qt::UserRole).toString();
    if (tag.isEmpty()) return;

    if (tag == "add_stage") {
        QMenu menu(this);
        for (StageCategory cat : {StageCategory::Volume, StageCategory::EQ, StageCategory::Dynamics, StageCategory::Delay, StageCategory::Matrix}) {
            QMenu* catMenu = menu.addMenu(QString::fromStdString(stageCategoryToString(cat)));
            for (StageType st : {
                StageType::Balance, StageType::Width, StageType::MSProc, StageType::PhaseInvert, StageType::Crossfeed, StageType::SplitWidth,
                StageType::EQ, StageType::GraphicEQ, StageType::Convolution, StageType::Loudness, StageType::Emphasis, StageType::DCProtection,
                StageType::Gain, StageType::Delay, StageType::LookaheadLimiter, StageType::Limiter, StageType::Volume, StageType::MatrixMixer,
                StageType::Compressor, StageType::NoiseGate, StageType::RACE, StageType::Dither, StageType::DiffEq, StageType::BiquadCombo
            }) {
                if (stageTypeToCategory(st) == cat) {
                    QAction* act = catMenu->addAction(QString::fromStdString(stageTypeToString(st)));
                    connect(act, &QAction::triggered, [this, st]() {
                        m_pipeline->addStage(st);
                        size_t newIdx = m_pipeline->stages.size() - 1;
                        m_lastActiveTag = QString("stage_%1").arg(newIdx);
                        handleNavigationTag(m_lastActiveTag);
                    });
                }
            }
        }
        menu.exec(QCursor::pos());
    } else if (tag == "add_eq") {
        m_pipeline->addEQPreset();
        if (!m_pipeline->eqPresets.empty()) {
            m_lastActiveTag = QString("eq_%1").arg(m_pipeline->eqPresets.back().id.toString());
            handleNavigationTag(m_lastActiveTag);
        }
    } else if (tag == "auto_eq") {
        AutoEqPickerDlg dlg(m_pipeline, this);
        dlg.exec();
    } else if (tag == "oratory_eq") {
        OratoryPresetPickerDlg dlg(m_pipeline, this);
        dlg.exec();
    } else if (tag == "import_conv") {
        ConvolutionImportDlg dlg(m_pipeline, this);
        dlg.exec();
    } else if (tag == "room_correction") {
        RoomCorrectionDlg dlg(m_pipeline, this);
        dlg.exec();
    } else {
        m_lastActiveTag = tag;
        handleNavigationTag(tag);
    }
}

void MainWindow::handleNavigationTag(const QString& tag) {
    if (tag == "dashboard") {
        showCentralWidget(new DashboardView(m_monitoring, m_dspController, m_spectrumEngine, m_spectrogramEngine, m_vectorScopeEngine, this));
    } else if (tag == "devices") {
        showCentralWidget(new DevicePickerView(m_devices, m_settings, this));
    } else if (tag == "levels") {
        auto container = new QWidget(this);
        auto layout = new QVBoxLayout(container);
        layout->setContentsMargins(16, 16, 16, 16);
        auto cap = new LevelMeterView(container);
        auto pb = new LevelMeterView(container);
        layout->addWidget(cap);
        layout->addWidget(pb);
        connect(m_monitoring.get(), &MonitoringController::levelsUpdated, container, [this, cap, pb]() {
            const auto& st = m_monitoring->levelState;
            cap->setLevels(st.captureRms, st.capturePeak, "Capture Levels");
            pb->setLevels(st.playbackRms, st.playbackPeak, "Playback Levels");
        });
        showCentralWidget(container);
    } else if (tag == "spectrum") {
        showCentralWidget(new SpectrumDetailView(m_spectrumEngine, m_devices, this));
    } else if (tag == "spectroscope") {
        showCentralWidget(new SpectrogramDetailView(m_spectrogramEngine, m_devices, this));
    } else if (tag == "vectorscope") {
        showCentralWidget(new VectorScopeDetailView(m_vectorScopeEngine, this));
    } else if (tag == "analogVU") {
        showCentralWidget(new AnalogVUDetailView(m_monitoring, this));
    } else if (tag == "resampler") {
        showCentralWidget(new ResamplerDetailView(m_settings, this));
    } else if (tag == "general_settings") {
        showCentralWidget(new GeneralSettingsView(m_settings, m_monitoring, this));
    } else if (tag == "logs") {
        showCentralWidget(new ConsoleLogsView(this));
    } else if (tag.startsWith("stage_")) {
        size_t idx = tag.mid(6).toULongLong();
        showCentralWidget(new StageDetailView(idx, m_pipeline, m_dspController, this));
    } else if (tag.startsWith("conv_")) {
        QUuid id = QUuid::fromString(tag.mid(5));
        for (const auto& preset : m_pipeline->convPresets) {
            if (preset.id == id) {
                showCentralWidget(new ConvolutionPresetDetailView(preset, m_pipeline, this));
                break;
            }
        }
    } else if (tag.startsWith("eq_")) {
        QUuid id = QUuid::fromString(tag.mid(3));
        for (const auto& preset : m_pipeline->eqPresets) {
            if (preset.id == id) {
                showCentralWidget(new EQPresetDetailView(preset, m_pipeline, this));
                break;
            }
        }
    }
}

void MainWindow::onPipelineChanged() {
    refreshSidebarItems();
}

void MainWindow::onEngineStatusChanged(ProcessingState state) {
    if (state == ProcessingState::Running) {
        m_startStopBtn->setText("Stop Engine");
        m_startStopBtn->setStyleSheet("background-color: #ff3b30; color: white;");
    } else {
        m_startStopBtn->setText("Start Engine");
        m_startStopBtn->setStyleSheet("background-color: #34c759; color: white;");
    }
    m_sampleRateBadge->setText(QString("%1 Hz").arg(m_devices->captureConfig.sampleRate));
}
