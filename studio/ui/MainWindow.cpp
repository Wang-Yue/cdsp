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
#include <QCheckBox>
#include <QFrame>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QContextMenuEvent>
#include <cmath>

namespace {
class SidebarToggleRowWidget : public QWidget {
public:
    SidebarToggleRowWidget(QTreeWidget* tree, QTreeWidgetItem* item, const QString& title, bool isChecked, std::function<void(bool)> onToggle, std::function<void()> onRowClick, QWidget* parent = nullptr)
        : QWidget(parent), m_tree(tree), m_item(item), m_onToggle(onToggle), m_onRowClick(onRowClick) {
        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 1, 6, 1);
        layout->setSpacing(4);

        m_label = new QLabel(title, this);
        layout->addWidget(m_label);

        layout->addStretch();

        m_checkbox = new QCheckBox(this);
        m_checkbox->setFocusPolicy(Qt::NoFocus);
        m_checkbox->setChecked(isChecked);
        m_checkbox->setStyleSheet("QCheckBox::indicator { width: 14px; height: 14px; }");
        layout->addWidget(m_checkbox);

        connect(m_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_onToggle) m_onToggle(checked);
        });
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (m_tree && m_item) {
            m_tree->setCurrentItem(m_item);
        }
        if (m_onRowClick) m_onRowClick();
        QWidget::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        if (m_tree && m_item) {
            m_tree->setCurrentItem(m_item);
            QPoint pos = m_tree->viewport()->mapFromGlobal(event->globalPos());
            emit m_tree->customContextMenuRequested(pos);
        }
    }

private:
    QTreeWidget* m_tree;
    QTreeWidgetItem* m_item;
    QLabel* m_label;
    QCheckBox* m_checkbox;
    std::function<void(bool)> m_onToggle;
    std::function<void()> m_onRowClick;
};
}

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

    // Wire app state callbacks
    m_settings->onChanged = [this]() {
        m_devices->validateSampleRates();
        m_dspController->applyConfig();
    };
    connect(m_pipeline.get(), &PipelineStore::pipelineChanged, [this]() {
        m_dspController->applyConfig();
    });

    connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, &MainWindow::onPipelineChanged);
    connect(m_dspController.get(), &DSPEngineController::statusChanged, this, &MainWindow::onEngineStatusChanged);
    connect(m_devices.get(), &AudioDeviceManager::configChanged, this, [this]() {
        if (m_sampleRateBadge) {
            m_sampleRateBadge->setText(QString("%1 Hz").arg(m_devices->captureConfig.sampleRate));
        }
        updateStatusBar();
    });
    connect(m_settings.get(), &AudioSettings::settingsChanged, this, [this]() {
        updateMuteDisplay();
        updateVolumeDisplay();
        updateStatusBar();
        updateTheme();
    });

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
            size_t idx = tag.mid(6).toULongLong();
            QMenu menu(this);
            auto moveUp = menu.addAction("Move Up");
            moveUp->setEnabled(idx > 0);
            connect(moveUp, &QAction::triggered, [this, idx]() {
                m_pipeline->moveStage(idx, idx - 1);
            });

            auto moveDown = menu.addAction("Move Down");
            moveDown->setEnabled(idx < m_pipeline->stages.size() - 1);
            connect(moveDown, &QAction::triggered, [this, idx]() {
                m_pipeline->moveStage(idx, idx + 1);
            });

            menu.addSeparator();
            auto del = menu.addAction("Delete Stage");
            connect(del, &QAction::triggered, [this, idx]() {
                if (idx < m_pipeline->stages.size()) {
                    m_pipeline->deleteStage(m_pipeline->stages[idx].id);
                }
            });
            menu.exec(QCursor::pos());
        } else if (tag.startsWith("eq_")) {
            QUuid id = QUuid::fromString(tag.mid(3));
            QMenu menu(this);
            auto del = menu.addAction("Delete EQ Preset");
            connect(del, &QAction::triggered, [this, id]() {
                m_pipeline->deleteEQPreset(id);
            });
            menu.exec(QCursor::pos());
        } else if (tag.startsWith("conv_")) {
            QUuid id = QUuid::fromString(tag.mid(5));
            QMenu menu(this);
            auto del = menu.addAction("Delete Convolution Preset");
            connect(del, &QAction::triggered, [this, id]() {
                m_pipeline->deleteConvPreset(id);
            });
            menu.exec(QCursor::pos());
        }
    });

    // Right detail panel with persistent top CompactLevelMeterBar
    auto rightPanel = new QWidget(m_splitter);
    auto rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_compactMeterBar = new CompactLevelMeterBar(m_monitoring, m_dspController, rightPanel);
    rightLayout->addWidget(m_compactMeterBar);

    auto line = new QFrame(rightPanel);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setStyleSheet("color: rgba(255, 255, 255, 0.1);");
    rightLayout->addWidget(line);

    m_centralStack = new QStackedWidget(rightPanel);
    rightLayout->addWidget(m_centralStack);

    m_splitter->addWidget(m_sidebarTree);
    m_splitter->addWidget(rightPanel);
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
    auto setupNavAction = [this](const QKeySequence& seq, const QString& tag) {
        auto act = new QAction(this);
        act->setShortcut(seq);
        connect(act, &QAction::triggered, [this, tag]() { handleNavigationTag(tag); });
        addAction(act);
    };

    setupNavAction(QKeySequence("Ctrl+1"), "devices");
    setupNavAction(QKeySequence("Ctrl+2"), "dashboard");
    setupNavAction(QKeySequence("Ctrl+3"), "levels");
    setupNavAction(QKeySequence("Ctrl+4"), "spectrum");
    setupNavAction(QKeySequence("Ctrl+5"), "general_settings");

    auto actMini = new QAction(this);
    actMini->setShortcut(QKeySequence("Ctrl+M"));
    connect(actMini, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);
    addAction(actMini);

    auto actMute = new QAction(this);
    actMute->setShortcut(QKeySequence("Space"));
    connect(actMute, &QAction::triggered, [this]() {
        auto focusW = QApplication::focusWidget();
        if (focusW && (qobject_cast<QLineEdit*>(focusW) ||
                       qobject_cast<QAbstractSpinBox*>(focusW) ||
                       qobject_cast<QTextEdit*>(focusW) ||
                       qobject_cast<QPlainTextEdit*>(focusW))) {
            return;
        }
        toggleMute();
    });
    addAction(actMute);
}

void MainWindow::toggleMute() {
    bool currentMute = m_settings->getMuted(Fader::Main);
    m_dspController->setFaderMute(Fader::Main, !currentMute);
    updateMuteDisplay();
}

void MainWindow::updateMuteDisplay() {
    bool muted = m_settings->getMuted(Fader::Main);
    if (muted) {
        m_toolbarMuteBtn->setText("🔇 Muted");
        m_toolbarMuteBtn->setStyleSheet("background-color: #ff3b30; color: white; font-weight: bold; padding: 4px 8px; border-radius: 4px;");
        m_statusMuteLabel->setText("MUTED");
        m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #ff3b30; font-weight: bold;");
    } else {
        m_toolbarMuteBtn->setText("🔊 Mute");
        m_toolbarMuteBtn->setStyleSheet("background-color: #3a3a3c; color: white; font-weight: normal; padding: 4px 8px; border-radius: 4px;");
        m_statusMuteLabel->setText("Unmuted");
        m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #34c759; font-weight: bold;");
    }
}

void MainWindow::updateVolumeDisplay() {
    float gain = m_settings->getVolume(Fader::Main);
    m_headerVolumeSlider->blockSignals(true);
    m_headerVolumeSlider->setValue(static_cast<int>(std::round(gain * 2.0f)));
    m_headerVolumeSlider->blockSignals(false);

    m_gainValueLabel->setText(QString::asprintf("%+.1f dB", gain));
    if (gain > 0.0f) {
        m_gainValueLabel->setStyleSheet("font-family: monospace; font-weight: bold; color: #ff3b30; min-width: 65px;");
    } else {
        m_gainValueLabel->setStyleSheet("font-family: monospace; font-weight: bold; color: #34c759; min-width: 65px;");
    }
}

void MainWindow::updateStatusBar() {
    QString stateStr;
    switch (m_dspController->status) {
    case ProcessingState::Running: stateStr = "Running"; break;
    case ProcessingState::Starting: stateStr = "Starting"; break;
    case ProcessingState::Paused: stateStr = "Paused"; break;
    case ProcessingState::Stalled: stateStr = "Stalled"; break;
    case ProcessingState::Inactive: default: stateStr = "Inactive"; break;
    }
    m_statusStateLabel->setText(QString("State: %1").arg(stateStr));
    m_statusBufferLabel->setText(QString("Buffer: %1").arg(m_settings->chunkSize));

    QString presetName = "Default";
    if (!m_pipeline->eqPresets.empty()) {
        presetName = QString::fromStdString(m_pipeline->eqPresets.front().name);
    }
    m_statusActivePresetLabel->setText(QString("Preset: %1").arg(presetName));
    updateMuteDisplay();
}

void MainWindow::setupToolbar() {
    auto toolBar = addToolBar("Main Controls");
    toolBar->setMovable(false);

    m_startStopBtn = new QPushButton("Start Engine", this);
    m_startStopBtn->setStyleSheet("background-color: #34c759; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
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
    m_sampleRateBadge->setStyleSheet("color: #8e8e93; padding: 0 12px;");
    toolBar->addWidget(m_sampleRateBadge);

    toolBar->addSeparator();

    auto volLabel = new QLabel(" Volume:", this);
    toolBar->addWidget(volLabel);

    m_toolbarMuteBtn = new QPushButton("🔊 Mute", this);
    m_toolbarMuteBtn->setStyleSheet("background-color: #3a3a3c; color: white; padding: 4px 8px; border-radius: 4px;");
    connect(m_toolbarMuteBtn, &QPushButton::clicked, this, &MainWindow::toggleMute);
    toolBar->addWidget(m_toolbarMuteBtn);

    // 400px Volume slider (-60 to +20 dB in 0.5 dB steps: mapped to -120 to +40 int range)
    m_headerVolumeSlider = new QSlider(Qt::Horizontal, this);
    m_headerVolumeSlider->setRange(-120, 40);
    m_headerVolumeSlider->setValue(static_cast<int>(m_settings->getVolume(Fader::Main) * 2.0f));
    m_headerVolumeSlider->setFixedWidth(360);
    connect(m_headerVolumeSlider, &QSlider::valueChanged, [this](int val) {
        float db = val / 2.0f;
        m_dspController->setFaderVolume(Fader::Main, db);
        updateVolumeDisplay();
    });
    toolBar->addWidget(m_headerVolumeSlider);

    m_gainValueLabel = new QLabel("  0.0 dB", this);
    m_gainValueLabel->setFont(QFont("monospace", 11, QFont::Bold));
    updateVolumeDisplay();
    toolBar->addWidget(m_gainValueLabel);

    toolBar->addSeparator();

    auto miniPlayerBtn = new QPushButton("Floating MiniPlayer", this);
    miniPlayerBtn->setStyleSheet("padding: 4px 10px;");
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

    auto levelsItem = new QTreeWidgetItem(monGroup);
    levelsItem->setData(0, Qt::UserRole, "levels");
    auto levelsW = new SidebarToggleRowWidget(m_sidebarTree, levelsItem, "Level Meters", m_settings->showLevelMetersInDashboard, [this](bool c) {
        m_settings->showLevelMetersInDashboard = c;
        m_settings->savePreferences();
    }, [this, levelsItem]() { onSidebarItemClicked(levelsItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(levelsItem, 0, levelsW);

    auto specItem = new QTreeWidgetItem(monGroup);
    specItem->setData(0, Qt::UserRole, "spectrum");
    auto specW = new SidebarToggleRowWidget(m_sidebarTree, specItem, "Spectrum", m_settings->showSpectrumInDashboard, [this](bool c) {
        m_settings->showSpectrumInDashboard = c;
        m_settings->savePreferences();
    }, [this, specItem]() { onSidebarItemClicked(specItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(specItem, 0, specW);

    auto spectroItem = new QTreeWidgetItem(monGroup);
    spectroItem->setData(0, Qt::UserRole, "spectroscope");
    auto spectroW = new SidebarToggleRowWidget(m_sidebarTree, spectroItem, "Spectroscope", m_settings->showSpectrogramInDashboard, [this](bool c) {
        m_settings->showSpectrogramInDashboard = c;
        m_settings->savePreferences();
    }, [this, spectroItem]() { onSidebarItemClicked(spectroItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(spectroItem, 0, spectroW);

    auto vecItem = new QTreeWidgetItem(monGroup);
    vecItem->setData(0, Qt::UserRole, "vectorscope");
    auto vecW = new SidebarToggleRowWidget(m_sidebarTree, vecItem, "Vector Scope", m_settings->showVectorScopeInDashboard, [this](bool c) {
        m_settings->showVectorScopeInDashboard = c;
        m_settings->savePreferences();
    }, [this, vecItem]() { onSidebarItemClicked(vecItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(vecItem, 0, vecW);

    auto vuItem = new QTreeWidgetItem(monGroup);
    vuItem->setData(0, Qt::UserRole, "analogVU");
    auto vuW = new SidebarToggleRowWidget(m_sidebarTree, vuItem, "Analog VU", m_settings->showAnalogVUInDashboard, [this](bool c) {
        m_settings->showAnalogVUInDashboard = c;
        m_settings->savePreferences();
    }, [this, vuItem]() { onSidebarItemClicked(vuItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(vuItem, 0, vuW);

    auto logsItem = new QTreeWidgetItem(monGroup, {"Console Logs"});
    logsItem->setData(0, Qt::UserRole, "logs");
    auto settingsItem = new QTreeWidgetItem(monGroup, {"General Settings"});
    settingsItem->setData(0, Qt::UserRole, "general_settings");

    // 3. Pipeline Section
    auto pipeGroup = new QTreeWidgetItem(m_sidebarTree, {"Pipeline"});
    pipeGroup->setExpanded(true);

    auto resItem = new QTreeWidgetItem(pipeGroup);
    resItem->setData(0, Qt::UserRole, "resampler");
    auto resW = new SidebarToggleRowWidget(m_sidebarTree, resItem, "Resampler", m_settings->resamplerEnabled, [this](bool c) {
        m_settings->resamplerEnabled = c;
        m_settings->savePreferences();
        m_dspController->applyConfig();
    }, [this, resItem]() { onSidebarItemClicked(resItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(resItem, 0, resW);

    for (size_t i = 0; i < m_pipeline->stages.size(); ++i) {
        const auto& stage = m_pipeline->stages[i];
        auto sItem = new QTreeWidgetItem(pipeGroup);
        sItem->setData(0, Qt::UserRole, QString("stage_%1").arg(i));
        auto stageW = new SidebarToggleRowWidget(m_sidebarTree, sItem, QString::fromStdString(stage.name), stage.isEnabled, [this, i](bool c) {
            if (i < m_pipeline->stages.size()) {
                m_pipeline->stages[i].isEnabled = c;
                m_pipeline->save();
                m_dspController->applyConfig();
            }
        }, [this, sItem]() { onSidebarItemClicked(sItem, 0); }, m_sidebarTree);
        m_sidebarTree->setItemWidget(sItem, 0, stageW);
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
    if (!widget) return;
    if (m_centralStack->indexOf(widget) == -1) {
        m_centralStack->addWidget(widget);
    }
    m_centralStack->setCurrentWidget(widget);
}

void MainWindow::onSidebarItemClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item) return;

    QString tag = item->data(0, Qt::UserRole).toString();
    if (tag.isEmpty()) return;

    if (tag == "add_stage") {
        QMenu menu(this);
        for (StageCategory cat : {StageCategory::Filters, StageCategory::Mixer, StageCategory::Processors, StageCategory::Others}) {
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
    m_sidebarTree->blockSignals(true);
    for (int i = 0; i < m_sidebarTree->topLevelItemCount(); ++i) {
        auto topItem = m_sidebarTree->topLevelItem(i);
        for (int j = 0; j < topItem->childCount(); ++j) {
            auto child = topItem->child(j);
            if (child->data(0, Qt::UserRole).toString() == tag) {
                m_sidebarTree->setCurrentItem(child);
                break;
            }
        }
    }
    m_sidebarTree->blockSignals(false);

    if (m_pageCache.contains(tag) && m_pageCache[tag]) {
        showCentralWidget(m_pageCache[tag]);
        return;
    }

    QWidget* w = nullptr;
    if (tag == "dashboard") {
        w = new DashboardView(m_monitoring, m_dspController, m_spectrumEngine, m_spectrogramEngine, m_vectorScopeEngine, this);
    } else if (tag == "devices") {
        w = new DevicePickerView(m_devices, m_settings, this);
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
        w = container;
    } else if (tag == "spectrum") {
        w = new SpectrumDetailView(m_spectrumEngine, m_devices, this);
    } else if (tag == "spectroscope") {
        w = new SpectrogramDetailView(m_spectrogramEngine, m_devices, this);
    } else if (tag == "vectorscope") {
        w = new VectorScopeDetailView(m_vectorScopeEngine, this);
    } else if (tag == "analogVU") {
        w = new AnalogVUDetailView(m_monitoring, this);
    } else if (tag == "resampler") {
        w = new ResamplerDetailView(m_settings, m_devices, this);
    } else if (tag == "general_settings") {
        w = new GeneralSettingsView(m_settings, m_monitoring, this);
    } else if (tag == "logs") {
        w = new ConsoleLogsView(this);
    } else if (tag.startsWith("stage_")) {
        size_t idx = tag.mid(6).toULongLong();
        w = new StageDetailView(idx, m_pipeline, m_dspController, this);
    } else if (tag.startsWith("conv_")) {
        QUuid id = QUuid::fromString(tag.mid(5));
        for (const auto& preset : m_pipeline->convPresets) {
            if (preset.id == id) {
                w = new ConvolutionPresetDetailView(preset, m_pipeline, this);
                break;
            }
        }
    } else if (tag.startsWith("eq_")) {
        QUuid id = QUuid::fromString(tag.mid(3));
        for (const auto& preset : m_pipeline->eqPresets) {
            if (preset.id == id) {
                auto eqView = new EQPresetDetailView(preset, m_pipeline, this);
                eqView->setSpectrumEngine(m_spectrumEngine);
                w = eqView;
                break;
            }
        }
    }

    if (w) {
        m_pageCache[tag] = w;
        showCentralWidget(w);
    }
}

void MainWindow::onPipelineChanged() {
    QList<QString> keysToDestroy;
    for (auto it = m_pageCache.begin(); it != m_pageCache.end(); ++it) {
        if (it.key().startsWith("stage_") || it.key().startsWith("eq_") || it.key().startsWith("conv_")) {
            keysToDestroy.append(it.key());
        }
    }
    for (const auto& key : keysToDestroy) {
        QWidget* w = m_pageCache.take(key);
        if (w) {
            m_centralStack->removeWidget(w);
            w->deleteLater();
        }
    }

    refreshSidebarItems();

    if (m_lastActiveTag.startsWith("stage_") || m_lastActiveTag.startsWith("eq_") || m_lastActiveTag.startsWith("conv_")) {
        handleNavigationTag("dashboard");
    }
}

void MainWindow::onEngineStatusChanged(ProcessingState state) {
    switch (state) {
    case ProcessingState::Running:
        m_startStopBtn->setText("Stop Engine");
        m_startStopBtn->setStyleSheet("background-color: #ff3b30; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
        break;
    case ProcessingState::Starting:
        m_startStopBtn->setText("Starting...");
        m_startStopBtn->setStyleSheet("background-color: #ffcc00; color: black; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
        break;
    case ProcessingState::Paused:
        m_startStopBtn->setText("Paused (Click to Resume)");
        m_startStopBtn->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
        break;
    case ProcessingState::Stalled:
        m_startStopBtn->setText("Stalled (Click to Restart)");
        m_startStopBtn->setStyleSheet("background-color: #ff9500; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
        break;
    case ProcessingState::Inactive:
    default:
        m_startStopBtn->setText("Start Engine");
        m_startStopBtn->setStyleSheet("background-color: #34c759; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
        break;
    }
    m_sampleRateBadge->setText(QString("%1 Hz").arg(m_devices->captureConfig.sampleRate));
    updateStatusBar();
}
