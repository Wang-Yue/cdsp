#include "ui/MainWindow.h"

#include "ui/AnalogVUMeterView.h"
#include "ui/AutoEqPickerDlg.h"
#include "ui/ConsoleLogsView.h"
#include "ui/ConvolutionImportDlg.h"
#include "ui/ConvolutionPresetDetailView.h"
#include "ui/DashboardView.h"
#include "ui/DevicePickerView.h"
#include "ui/EQPresetDetailView.h"
#include "ui/GeneralSettingsView.h"
#include "ui/LevelMeterView.h"
#include "ui/OratoryPresetPickerDlg.h"
#include "ui/ResamplerDetailView.h"
#include "ui/RoomCorrectionDlg.h"
#include "ui/SpectrogramView.h"
#include "ui/SpectrumView.h"
#include "ui/StageDetailView.h"
#include "ui/VectorScopeView.h"
#include "ui/VisualizerDetailViews.h"
#include "utils/AppIcon.h"
#include "utils/MacUtils.h"
#include "utils/ThemeManager.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>

namespace {
bool isInputWidgetFocused() {
    QWidget* focusW = QApplication::focusWidget();
    for (QWidget* w = focusW; w; w = w->parentWidget()) {
        if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QSpinBox*>(w) ||
            qobject_cast<QDoubleSpinBox*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) ||
            qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractButton*>(w)) {
            return true;
        }
    }
    return false;
}
class SidebarToggleRowWidget : public QWidget {
public:
    SidebarToggleRowWidget(QTreeWidget* tree, QTreeWidgetItem* item, const QString& title, bool isChecked,
                           std::function<void(bool)> onToggle, std::function<void()> onRowClick,
                           QWidget* parent = nullptr)
        : QWidget(parent), m_tree(tree), m_item(item), m_onToggle(onToggle), m_onRowClick(onRowClick) {
        setFixedHeight(26);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (m_item) {
            m_item->setSizeHint(0, QSize(0, 26));
        }

        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 0, 8, 0);
        layout->setSpacing(6);

        m_label = new QLabel(title, this);
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(m_label);

        layout->addStretch();

        m_checkbox = new QCheckBox(this);
        m_checkbox->setFocusPolicy(Qt::NoFocus);
        m_checkbox->setChecked(isChecked);
        m_checkbox->setCursor(Qt::PointingHandCursor);
        m_checkbox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(m_checkbox);

        connect(m_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_onToggle)
                m_onToggle(checked);
        });
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (m_tree && m_item) {
            m_tree->setCurrentItem(m_item);
        }
        if (m_onRowClick)
            m_onRowClick();
        event->accept();
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
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_engine = std::make_shared<CDSPEngine>();
    LogManager::instance()->setEngine(m_engine.get());
    m_settings = std::make_shared<AudioSettings>();
    m_devices = std::make_shared<AudioDeviceManager>(m_engine, m_settings);
    m_pipeline = std::make_shared<PipelineStore>();
    m_spectrumEngine = std::make_shared<SpectrumEngine>();
    m_spectrogramEngine = std::make_shared<SpectrogramEngine>();
    m_vectorScopeEngine = std::make_shared<VectorScopeEngine>();
    auto levelStatePtr = std::make_shared<LevelState>();

    m_monitoring = std::make_shared<MonitoringController>(
        m_engine, levelStatePtr, m_spectrumEngine, m_spectrogramEngine, m_vectorScopeEngine, m_devices, m_settings);

    m_dspController =
        std::make_shared<DSPEngineController>(m_engine, m_devices, m_settings, m_pipeline, m_monitoring, levelStatePtr);

    m_miniPlayer = std::make_unique<MiniPlayerView>(m_dspController, m_settings, m_monitoring);
    connect(m_miniPlayer.get(), &MiniPlayerView::requestRestoreMainWindow, this, &MainWindow::showAndActivate);

    resize(1100, 780);
    setMinimumSize(960, 680);
    setWindowTitle("DSP Monitor");
    setWindowIcon(AppIcon::getAppIcon());
    setWindowFlag(Qt::WindowFullscreenButtonHint, false);
    MacUtils::disableFullScreen(this);
    MacUtils::setupMinimizeToTray(this);

    setupUi();
    setupMenuBar();
    setupStatusBar();
    setupTrayIcon();
    setupShortcuts();

    // Wire app state callbacks
    m_settings->onChanged = [this]() {
        m_devices->validateSampleRates();
        m_dspController->applyConfig();
    };
    m_devices->onConfigChanged = [this]() { m_dspController->applyConfig(); };

    connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, &MainWindow::onPipelineChanged);
    connect(m_dspController.get(), &DSPEngineController::statusChanged, this, &MainWindow::onEngineStatusChanged);
    connect(m_dspController.get(), &DSPEngineController::statusUpdated, this,
            [this](ProcessingState state, const ProcessingStopReason& stopReason) {
                if (stopReason.type != StopReasonType::None && stopReason.type != StopReasonType::Done) {
                    QString msg;
                    switch (stopReason.type) {
                    case StopReasonType::CaptureFormatChange:
                        msg = QString("⚠️ Format Change: Capture sample rate changed to %1 Hz")
                                  .arg(stopReason.formatChangeRate);
                        break;
                    case StopReasonType::PlaybackFormatChange:
                        msg = QString("⚠️ Format Change: Playback sample rate changed to %1 Hz")
                                  .arg(stopReason.formatChangeRate);
                        break;
                    case StopReasonType::CaptureError:
                        msg = QString("⚠️ Capture Error: %1").arg(QString::fromStdString(stopReason.message));
                        break;
                    case StopReasonType::PlaybackError:
                        msg = QString("⚠️ Playback Error: %1").arg(QString::fromStdString(stopReason.message));
                        break;
                    case StopReasonType::UnknownError:
                        msg = QString("⚠️ Error: %1").arg(QString::fromStdString(stopReason.message));
                        break;
                    default:
                        break;
                    }
                    if (!msg.isEmpty() && m_stopReasonBanner) {
                        m_stopReasonBanner->setText(msg);
                        m_stopReasonBanner->show();
                    }
                } else if ((state == ProcessingState::Running || stopReason.type == StopReasonType::None ||
                            stopReason.type == StopReasonType::Done) &&
                           m_stopReasonBanner) {
                    m_stopReasonBanner->hide();
                }
            });

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
    });
    connect(m_settings.get(), &AudioSettings::fadersChanged, this, [this]() {
        updateMuteDisplay();
        updateVolumeDisplay();
    });

    m_monitoring->start();

    // Save state on application shutdown
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (m_pipeline) {
            m_pipeline->save();
        }
        if (m_settings) {
            m_settings->savePreferences();
        }
    });

    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](AppTheme theme, bool isDark) {
        Q_UNUSED(theme);
        Q_UNUSED(isDark);
        refreshSidebarItems();
    });
}

void MainWindow::setupUi() {
    setupToolbar();

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName("MainSplitter");
    m_splitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    setCentralWidget(m_splitter);

    m_sidebarTree = new QTreeWidget(m_splitter);
    m_sidebarTree->setObjectName("SidebarTree");
    m_sidebarTree->setHeaderHidden(true);
    m_sidebarTree->setRootIsDecorated(true);
    m_sidebarTree->setUniformRowHeights(true);
    m_sidebarTree->setIndentation(12);
    m_sidebarTree->setAnimated(true);
    m_sidebarTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_sidebarTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sidebarTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sidebarTree->setMinimumWidth(220);
    m_sidebarTree->setMaximumWidth(360);
    m_sidebarTree->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_sidebarTree->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_sidebarTree, &QTreeWidget::itemClicked, this, &MainWindow::onSidebarItemClicked);
    connect(m_sidebarTree, &QTreeWidget::customContextMenuRequested, [this](const QPoint& pos) {
        auto item = m_sidebarTree->itemAt(pos);
        if (!item)
            return;
        QString tag = item->data(0, Qt::UserRole).toString();

        if (tag.startsWith("stage_")) {
            QUuid stageId = QUuid::fromString(tag.mid(6));
            int idx = -1;
            for (size_t i = 0; i < m_pipeline->stages.size(); ++i) {
                if (m_pipeline->stages[i].id == stageId) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            if (idx >= 0) {
                QMenu menu(this);
                auto moveUp = menu.addAction("Move Up");
                moveUp->setEnabled(idx > 0);
                connect(moveUp, &QAction::triggered, [this, idx]() { m_pipeline->moveStage(idx, idx - 1); });

                auto moveDown = menu.addAction("Move Down");
                moveDown->setEnabled(idx < static_cast<int>(m_pipeline->stages.size()) - 1);
                connect(moveDown, &QAction::triggered, [this, idx]() { m_pipeline->moveStage(idx, idx + 1); });

                menu.addSeparator();
                auto del = menu.addAction("Delete Stage");
                connect(del, &QAction::triggered, [this, stageId]() { m_pipeline->deleteStage(stageId); });
                menu.exec(QCursor::pos());
            }
        } else if (tag.startsWith("eq_")) {
            QUuid id = QUuid::fromString(tag.mid(3));
            QMenu menu(this);
            auto del = menu.addAction("Delete EQ Preset");
            connect(del, &QAction::triggered, [this, id]() { m_pipeline->deleteEQPreset(id); });
            menu.exec(QCursor::pos());
        } else if (tag.startsWith("conv_")) {
            QUuid id = QUuid::fromString(tag.mid(5));
            QMenu menu(this);
            auto del = menu.addAction("Delete Convolution Preset");
            connect(del, &QAction::triggered, [this, id]() { m_pipeline->deleteConvPreset(id); });
            menu.exec(QCursor::pos());
        }
    });

    // Detail container on the right side of splitter (matching CamillaDSP-Monitor)
    auto detailContainer = new QWidget(m_splitter);
    detailContainer->setObjectName("DetailContainer");
    auto detailLayout = new QVBoxLayout(detailContainer);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(0);

    // Top Header: Compact Level Meters + Stop Banner + Status Indicator
    auto headerWidget = new QWidget(detailContainer);
    headerWidget->setObjectName("DetailHeaderWidget");
    auto headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 8, 16, 8);
    headerLayout->setSpacing(12);

    m_compactMeterBar = new CompactLevelMeterBar(m_monitoring, m_dspController, headerWidget);
    m_compactMeterBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    headerLayout->addWidget(m_compactMeterBar, 1);

    m_stopReasonBanner = new QLabel(headerWidget);
    m_stopReasonBanner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_stopReasonBanner->setStyleSheet("color: #ff3b30; font-weight: bold; padding: 0 4px;");
    m_stopReasonBanner->hide();
    headerLayout->addWidget(m_stopReasonBanner);

    m_statusStateLabel = new QLabel("🔴 Inactive", headerWidget);
    m_statusStateLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLayout->addWidget(m_statusStateLabel);

    detailLayout->addWidget(headerWidget);

    // Subtle horizontal divider line below header bar
    auto divider = new QFrame(detailContainer);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setStyleSheet("color: rgba(128, 128, 128, 0.25);");
    divider->setFixedHeight(1);
    detailLayout->addWidget(divider);

    // Central detail stack
    m_centralStack = new QStackedWidget(detailContainer);
    m_centralStack->setObjectName("CentralStack");
    m_centralStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    detailLayout->addWidget(m_centralStack, 1);

    m_splitter->addWidget(m_sidebarTree);
    m_splitter->addWidget(detailContainer);
    m_splitter->setSizes({260, 840});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, false);

    setupSidebar();
}

void MainWindow::setupStatusBar() {
    statusBar()->hide();
    updateStatusBar();
}

void MainWindow::setupMenuBar() {
    auto bar = menuBar();
    // 1. File Menu
    auto fileMenu = bar->addMenu("&File");

    m_actAddEqPreset = new QAction("New EQ Preset", this);
    m_actAddEqPreset->setShortcut(QKeySequence::New);
    connect(m_actAddEqPreset, &QAction::triggered, [this]() {
        m_pipeline->addEQPreset();
        if (!m_pipeline->eqPresets.empty()) {
            m_lastActiveTag = QString("eq_%1").arg(m_pipeline->eqPresets.back().id.toString());
            handleNavigationTag(m_lastActiveTag);
        }
    });
    fileMenu->addAction(m_actAddEqPreset);

    m_actOratoryPreset = new QAction("Oratory Presets...", this);
    connect(m_actOratoryPreset, &QAction::triggered, [this]() {
        OratoryPresetPickerDlg dlg(m_pipeline, m_dspController, this);
        dlg.exec();
    });
    fileMenu->addAction(m_actOratoryPreset);

    m_actAutoEqPreset = new QAction("AutoEQ Presets...", this);
    connect(m_actAutoEqPreset, &QAction::triggered, [this]() {
        AutoEqPickerDlg dlg(m_pipeline, m_dspController, this);
        dlg.exec();
    });
    fileMenu->addAction(m_actAutoEqPreset);

    fileMenu->addSeparator();

    m_actImportConv = new QAction("Import IR File(s)...", this);
    m_actImportConv->setShortcut(QKeySequence::Open);
    connect(m_actImportConv, &QAction::triggered, [this]() {
        ConvolutionImportDlg dlg(m_pipeline, this);
        dlg.exec();
    });
    fileMenu->addAction(m_actImportConv);

    m_actRoomCorrection = new QAction("Room Correction...", this);
    connect(m_actRoomCorrection, &QAction::triggered, [this]() {
        RoomCorrectionDlg dlg(m_pipeline, this);
        dlg.exec();
    });
    fileMenu->addAction(m_actRoomCorrection);

    fileMenu->addSeparator();

    auto importPipelineAct = new QAction("Import Pipeline Configuration...", this);
    importPipelineAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(importPipelineAct, &QAction::triggered, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Import Pipeline Configuration", QString(),
                                                    "JSON Files (*.json);;All Files (*)");
        if (path.isEmpty())
            return;

        if (m_pipeline->importFromJsonFile(path)) {
            m_dspController->applyConfig();
            onPipelineChanged();
            QMessageBox::information(this, "Pipeline Imported",
                                     "Successfully imported pipeline configuration and presets.");
        } else {
            QMessageBox::critical(
                this, "Import Failed",
                "Failed to import pipeline configuration. Please ensure the file is a valid JSON configuration.");
        }
    });
    fileMenu->addAction(importPipelineAct);

    auto exportPipelineAct = new QAction("Export Pipeline Configuration...", this);
    exportPipelineAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(exportPipelineAct, &QAction::triggered, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "Export Pipeline Configuration", "pipeline_config.json",
                                                    "JSON Files (*.json);;All Files (*)");
        if (path.isEmpty())
            return;

        if (m_pipeline->exportToJsonFile(path)) {
            QMessageBox::information(this, "Pipeline Exported",
                                     QString("Successfully exported pipeline configuration to:\n%1").arg(path));
        } else {
            QMessageBox::critical(this, "Export Failed", "Failed to write pipeline configuration to file.");
        }
    });
    fileMenu->addAction(exportPipelineAct);

    auto exportCdspConfigAct = new QAction("Export CDSP Engine Config...", this);
    connect(exportCdspConfigAct, &QAction::triggered, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "Export CDSP Engine Config", "cdsp_config.json",
                                                    "JSON Files (*.json);;YAML Files (*.yml *.yaml);;All Files (*)");
        if (path.isEmpty())
            return;

        auto config = m_dspController->buildConfiguration();
        std::string configStr = config.toJsonString();
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(configStr.data(), configStr.size());
            file.close();
            QMessageBox::information(this, "Config Exported",
                                     QString("Successfully exported CDSP configuration to:\n%1").arg(path));
        } else {
            QMessageBox::critical(this, "Export Failed", "Failed to write CDSP configuration to file.");
        }
    });
    fileMenu->addAction(exportCdspConfigAct);

    fileMenu->addSeparator();

    auto closeAct = new QAction("Close Window", this);
    closeAct->setShortcut(QKeySequence::Close);
    connect(closeAct, &QAction::triggered, this, &QMainWindow::close);
    fileMenu->addAction(closeAct);

    fileMenu->addSeparator();

    auto settingsAct = new QAction("Settings...", this);
    settingsAct->setMenuRole(QAction::PreferencesRole);
    settingsAct->setShortcut(QKeySequence::Preferences);
    connect(settingsAct, &QAction::triggered, [this]() { handleNavigationTag("general_settings"); });
    fileMenu->addAction(settingsAct);

    auto aboutAct = new QAction("About CDSP Monitor", this);
    aboutAct->setMenuRole(QAction::AboutRole);
    connect(aboutAct, &QAction::triggered, [this]() {
        QMessageBox::about(this, "About CDSP Monitor",
                           "CDSP Monitor\n\nA cross-platform Qt audio DSP monitoring and pipeline controller.");
    });
    fileMenu->addAction(aboutAct);

    auto quitAct = new QAction("Quit CDSP Monitor", this);
    quitAct->setShortcut(QKeySequence::Quit);
    quitAct->setMenuRole(QAction::QuitRole);
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);
    fileMenu->addAction(quitAct);

    // 2. View Menu
    auto viewMenu = bar->addMenu("&View");
    auto setupViewAct = [this, viewMenu](const QString& title, const QKeySequence& seq, const QString& tag) {
        auto act = new QAction(title, this);
        act->setMenuRole(QAction::NoRole);
        act->setShortcut(seq);
        connect(act, &QAction::triggered, [this, tag]() { handleNavigationTag(tag); });
        viewMenu->addAction(act);
    };

    setupViewAct("Devices", QKeySequence(Qt::CTRL | Qt::Key_1), "devices");
    setupViewAct("Dashboard", QKeySequence(Qt::CTRL | Qt::Key_2), "dashboard");
    setupViewAct("Level Meters", QKeySequence(Qt::CTRL | Qt::Key_3), "levels");
    setupViewAct("Spectrum", QKeySequence(Qt::CTRL | Qt::Key_4), "spectrum");
    setupViewAct("Spectroscope Waterfall", QKeySequence(Qt::CTRL | Qt::Key_5), "spectroscope");
    setupViewAct("Vector Scope", QKeySequence(Qt::CTRL | Qt::Key_6), "vectorscope");
    setupViewAct("Analog VU Meter", QKeySequence(Qt::CTRL | Qt::Key_7), "analogVU");
    setupViewAct("Console Logs", QKeySequence(Qt::CTRL | Qt::Key_8), "logs");
    setupViewAct("General Settings", QKeySequence(Qt::CTRL | Qt::Key_9), "general_settings");

    viewMenu->addSeparator();

    auto miniAct = new QAction("Toggle MiniPlayer", this);
    miniAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    connect(miniAct, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);
    viewMenu->addAction(miniAct);

    // 3. Audio Menu
    auto audioMenu = bar->addMenu("&Audio");
    m_actStartStop = new QAction("Start/Stop Engine", this);
    m_actStartStop->setShortcut(QKeySequence(Qt::Key_Space));
    connect(m_actStartStop, &QAction::triggered, [this]() {
        if (isInputWidgetFocused()) {
            return;
        }
        if (m_dspController->status == ProcessingState::Running) {
            m_dspController->stopEngine();
        } else {
            m_dspController->startEngine();
        }
    });
    audioMenu->addAction(m_actStartStop);

    m_actMute = new QAction("Toggle Mute", this);
    m_actMute->setShortcut(QKeySequence(Qt::Key_M));
    connect(m_actMute, &QAction::triggered, [this]() {
        if (isInputWidgetFocused()) {
            return;
        }
        toggleMute();
    });
    audioMenu->addAction(m_actMute);

    // 4. Window Menu
    auto windowMenu = bar->addMenu("&Window");
    auto minAct = new QAction("Minimize", this);
    minAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(minAct, &QAction::triggered, this, &MainWindow::showMinimized);
    windowMenu->addAction(minAct);

    auto zoomAct = new QAction("Zoom", this);
    connect(zoomAct, &QAction::triggered, [this]() {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    windowMenu->addAction(zoomAct);

    windowMenu->addSeparator();

    auto bringAllAct = new QAction("Bring All to Front", this);
    connect(bringAllAct, &QAction::triggered, this, &MainWindow::showAndActivate);
    windowMenu->addAction(bringAllAct);

    // 5. Help Menu
    auto helpMenu = bar->addMenu("&Help");
    auto helpAct = new QAction("CDSP Monitor Help", this);
    connect(helpAct, &QAction::triggered,
            [this]() { QDesktopServices::openUrl(QUrl("https://github.com/Wang-Yue/Monitor-Qt")); });
    helpMenu->addAction(helpAct);
}

void MainWindow::setupTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(AppIcon::getAppIcon());
    m_trayIcon->setToolTip("CDSP Monitor");

    m_trayMenu = new QMenu(this);

    auto showAct = m_trayMenu->addAction("Show Main Window");
    connect(showAct, &QAction::triggered, this, [this]() {
        if (m_miniPlayer && m_miniPlayer->isVisible()) {
            m_miniPlayer->hide();
        }
        showAndActivate();
    });

    auto miniPlayerAct = m_trayMenu->addAction("Toggle MiniPlayer");
    connect(miniPlayerAct, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);

    m_trayMenu->addSeparator();

    m_trayStartStopAction = m_trayMenu->addAction("▶ Start Engine");
    connect(m_trayStartStopAction, &QAction::triggered, this, [this]() {
        if (m_dspController->status == ProcessingState::Running || m_dspController->status == ProcessingState::Paused ||
            m_dspController->status == ProcessingState::Stalled) {
            m_dspController->stopEngine();
        } else {
            m_dspController->startEngine();
        }
    });

    m_trayMuteAction = m_trayMenu->addAction("🔇 Mute");
    m_trayMuteAction->setCheckable(true);
    connect(m_trayMuteAction, &QAction::triggered, this, &MainWindow::toggleMute);

    m_trayMenu->addSeparator();

    auto quitAct = m_trayMenu->addAction("Quit CDSP Monitor");
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->show();

    updateTrayMenu();
}

void MainWindow::updateTrayMenu() {
    if (m_trayStartStopAction) {
        bool isRunning =
            (m_dspController->status == ProcessingState::Running ||
             m_dspController->status == ProcessingState::Paused || m_dspController->status == ProcessingState::Stalled);
        m_trayStartStopAction->setText(isRunning ? "⏹ Stop Engine" : "▶ Start Engine");
    }

    if (m_trayMuteAction) {
        bool muted = m_settings->getMuted(Fader::Main);
        m_trayMuteAction->setChecked(muted);
        m_trayMuteAction->setText(muted ? "🔊 Unmute" : "🔇 Mute");
    }
}

void MainWindow::setupShortcuts() {
    // Esc Key to exit miniplayer or reset focus / return to dashboard
    auto actEsc = new QAction(this);
    actEsc->setShortcut(QKeySequence("Esc"));
    connect(actEsc, &QAction::triggered, [this]() {
        if (m_miniPlayer && m_miniPlayer->isVisible()) {
            m_miniPlayer->hide();
            showAndActivate();
        } else {
            auto focusW = QApplication::focusWidget();
            if (focusW && !qobject_cast<QMainWindow*>(focusW)) {
                focusW->clearFocus();
            } else if (m_lastActiveTag != "dashboard") {
                handleNavigationTag("dashboard");
            }
        }
    });
    addAction(actEsc);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    MacUtils::showDockIcon();
    MacUtils::disableFullScreen(this);
    MacUtils::setupMinimizeToTray(this);
}

void MainWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    MacUtils::hideDockIcon();
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        if (isFullScreen()) {
            showNormal();
            return;
        }
        if (isMinimized()) {
            QTimer::singleShot(0, this, [this]() {
                if (isMinimized()) {
                    hide();
                }
            });
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    hide();
    event->ignore();
}

void MainWindow::toggleMute() {
    bool currentMute = m_settings->getMuted(Fader::Main);
    m_dspController->setFaderMute(Fader::Main, !currentMute);
    updateMuteDisplay();
}

void MainWindow::updateMuteDisplay() {
    bool muted = m_settings->getMuted(Fader::Main);
    m_toolbarMuteBtn->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    m_toolbarMuteBtn->setText("");

    if (m_trayMuteAction) {
        m_trayMuteAction->setChecked(muted);
        m_trayMuteAction->setText(muted ? "🔊 Unmute" : "🔇 Mute");
    }
}

void MainWindow::updateVolumeDisplay() {
    float gain = m_settings->getVolume(Fader::Main);
    if (!m_headerVolumeSlider->isSliderDown()) {
        m_headerVolumeSlider->blockSignals(true);
        m_headerVolumeSlider->setValue(static_cast<int>(std::round(gain * 2.0f)));
        m_headerVolumeSlider->blockSignals(false);
    }

    m_gainValueLabel->setText(QString::asprintf("%+.1f dB", gain));
    if (gain > 0.0f) {
        m_gainValueLabel->setStyleSheet("color: #ff3b30; font-weight: bold;");
    } else {
        m_gainValueLabel->setStyleSheet("");
    }
}

void MainWindow::updateStatusBar() {
    QString stateStr;
    switch (m_dspController->status) {
    case ProcessingState::Running:
        stateStr = "🟢 Running";
        break;
    case ProcessingState::Starting:
        stateStr = "🟡 Starting";
        break;
    case ProcessingState::Paused:
        stateStr = "🟡 Paused";
        break;
    case ProcessingState::Stalled:
        stateStr = "🟡 Stalled";
        break;
    case ProcessingState::Inactive:
    default:
        stateStr = "🔴 Inactive";
        break;
    }
    if (m_statusStateLabel) {
        m_statusStateLabel->setText(stateStr);
    }
    updateMuteDisplay();
}

void MainWindow::onEngineStatusChanged(ProcessingState state) {
    switch (state) {
    case ProcessingState::Running:
    case ProcessingState::Paused:
    case ProcessingState::Stalled:
        m_startStopBtn->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        m_startStopBtn->setText("Stop");
        m_startStopBtn->setToolTip("Stop Engine");
        break;
    case ProcessingState::Starting:
        m_startStopBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        m_startStopBtn->setText("Starting...");
        m_startStopBtn->setToolTip("Starting Engine...");
        break;
    case ProcessingState::Inactive:
    default:
        m_startStopBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        m_startStopBtn->setText("Start");
        m_startStopBtn->setToolTip("Start Engine");
        break;
    }
    if (m_sampleRateBadge) {
        m_sampleRateBadge->setText(QString("%1 Hz").arg(m_devices->captureConfig.sampleRate));
    }
    updateStatusBar();
    updateTrayMenu();
}

void MainWindow::setupToolbar() {
    auto toolBar = addToolBar("Main Controls");
    toolBar->setObjectName("MainToolBar");
    toolBar->setMovable(false);
    toolBar->setFloatable(false);
    toolBar->setIconSize(QSize(18, 18));

    m_startStopBtn = new QPushButton("Start", this);
    m_startStopBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_startStopBtn->setToolTip("Start Engine (Space)");
    m_startStopBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_startStopBtn->setMinimumWidth(88);
    m_startStopBtn->setCursor(Qt::PointingHandCursor);
    connect(m_startStopBtn, &QPushButton::clicked, [this]() {
        if (m_dspController->status == ProcessingState::Running || m_dspController->status == ProcessingState::Paused ||
            m_dspController->status == ProcessingState::Stalled) {
            m_dspController->stopEngine();
        } else {
            m_dspController->startEngine();
        }
    });
    toolBar->addWidget(m_startStopBtn);

    m_sampleRateBadge = new QLabel("48000 Hz", this);
    m_sampleRateBadge->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_sampleRateBadge->setAlignment(Qt::AlignCenter);
    m_sampleRateBadge->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_sampleRateBadge->setContentsMargins(8, 2, 8, 2);
    toolBar->addWidget(m_sampleRateBadge);

    toolBar->addSeparator();

    m_toolbarMuteBtn = new QPushButton(this);
    m_toolbarMuteBtn->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_toolbarMuteBtn->setToolTip("Toggle Mute (M)");
    m_toolbarMuteBtn->setFlat(true);
    m_toolbarMuteBtn->setFixedSize(28, 28);
    m_toolbarMuteBtn->setCursor(Qt::PointingHandCursor);
    m_toolbarMuteBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_toolbarMuteBtn, &QPushButton::clicked, this, &MainWindow::toggleMute);
    toolBar->addWidget(m_toolbarMuteBtn);

    // Volume slider (-60 to +20 dB in 0.5 dB steps: mapped to -120 to +40 int range)
    m_headerVolumeSlider = new QSlider(Qt::Horizontal, this);
    m_headerVolumeSlider->setRange(-120, 40);
    m_headerVolumeSlider->setSingleStep(1);
    m_headerVolumeSlider->setPageStep(2);
    m_headerVolumeSlider->setValue(static_cast<int>(m_settings->getVolume(Fader::Main) * 2.0f));
    m_headerVolumeSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_headerVolumeSlider->setMinimumWidth(200);
    m_headerVolumeSlider->setMaximumWidth(450);
    m_headerVolumeSlider->setToolTip("Master Output Volume (-60 dB to +20 dB)");
    connect(m_headerVolumeSlider, &QSlider::valueChanged, [this](int val) {
        float db = val / 2.0f;
        m_dspController->setFaderVolume(Fader::Main, db);
        updateVolumeDisplay();
    });
    toolBar->addWidget(m_headerVolumeSlider);

    m_gainValueLabel = new QLabel("  0.0 dB", this);
    m_gainValueLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_gainValueLabel->setMinimumWidth(75);
    m_gainValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_gainValueLabel->setContentsMargins(0, 0, 10, 0);
    m_gainValueLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    updateVolumeDisplay();
    toolBar->addWidget(m_gainValueLabel);
}

void MainWindow::toggleMiniPlayer() {
    if (m_miniPlayer->isVisible()) {
        m_miniPlayer->hide();
        showAndActivate();
    } else {
        hide();
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

    auto makeCategoryItem = [](QTreeWidget* tree, const QString& title) {
        auto item = new QTreeWidgetItem(tree, {title});
        item->setExpanded(true);
        item->setFlags(Qt::ItemIsEnabled);
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setSizeHint(0, QSize(0, 24));
        return item;
    };

    // 1. Audio Section
    auto audioGroup = makeCategoryItem(m_sidebarTree, "Audio");
    auto devItem = new QTreeWidgetItem(audioGroup, {"Devices"});
    devItem->setData(0, Qt::UserRole, "devices");
    devItem->setSizeHint(0, QSize(0, 26));
    auto dashItem = new QTreeWidgetItem(audioGroup, {"Dashboard"});
    dashItem->setData(0, Qt::UserRole, "dashboard");
    dashItem->setSizeHint(0, QSize(0, 26));

    // 2. Monitoring Section
    auto monGroup = makeCategoryItem(m_sidebarTree, "Monitoring");

    auto levelsItem = new QTreeWidgetItem(monGroup);
    levelsItem->setData(0, Qt::UserRole, "levels");
    auto levelsW = new SidebarToggleRowWidget(
        m_sidebarTree, levelsItem, "Level Meters", m_settings->showLevelMetersInDashboard,
        [this](bool c) {
            m_settings->showLevelMetersInDashboard = c;
            m_settings->savePreferences();
        },
        [this, levelsItem]() { onSidebarItemClicked(levelsItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(levelsItem, 0, levelsW);

    auto specItem = new QTreeWidgetItem(monGroup);
    specItem->setData(0, Qt::UserRole, "spectrum");
    auto specW = new SidebarToggleRowWidget(
        m_sidebarTree, specItem, "Spectrum", m_settings->showSpectrumInDashboard,
        [this](bool c) {
            m_settings->showSpectrumInDashboard = c;
            m_settings->savePreferences();
        },
        [this, specItem]() { onSidebarItemClicked(specItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(specItem, 0, specW);

    auto spectroItem = new QTreeWidgetItem(monGroup);
    spectroItem->setData(0, Qt::UserRole, "spectroscope");
    auto spectroW = new SidebarToggleRowWidget(
        m_sidebarTree, spectroItem, "Spectroscope Waterfall", m_settings->showSpectrogramInDashboard,
        [this](bool c) {
            m_settings->showSpectrogramInDashboard = c;
            m_settings->savePreferences();
        },
        [this, spectroItem]() { onSidebarItemClicked(spectroItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(spectroItem, 0, spectroW);

    auto vecItem = new QTreeWidgetItem(monGroup);
    vecItem->setData(0, Qt::UserRole, "vectorscope");
    auto vecW = new SidebarToggleRowWidget(
        m_sidebarTree, vecItem, "Vector Scope", m_settings->showVectorScopeInDashboard,
        [this](bool c) {
            m_settings->showVectorScopeInDashboard = c;
            m_settings->savePreferences();
        },
        [this, vecItem]() { onSidebarItemClicked(vecItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(vecItem, 0, vecW);

    auto vuItem = new QTreeWidgetItem(monGroup);
    vuItem->setData(0, Qt::UserRole, "analogVU");
    auto vuW = new SidebarToggleRowWidget(
        m_sidebarTree, vuItem, "Analog VU", m_settings->showAnalogVUInDashboard,
        [this](bool c) {
            m_settings->showAnalogVUInDashboard = c;
            m_settings->savePreferences();
        },
        [this, vuItem]() { onSidebarItemClicked(vuItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(vuItem, 0, vuW);

    auto logsItem = new QTreeWidgetItem(monGroup, {"Console Logs"});
    logsItem->setData(0, Qt::UserRole, "logs");
    logsItem->setSizeHint(0, QSize(0, 26));

    // 3. Pipeline Section
    auto pipeGroup = makeCategoryItem(m_sidebarTree, "Pipeline");

    auto resItem = new QTreeWidgetItem(pipeGroup);
    resItem->setData(0, Qt::UserRole, "resampler");
    auto resW = new SidebarToggleRowWidget(
        m_sidebarTree, resItem, "Resampler", m_settings->resamplerEnabled,
        [this](bool c) {
            m_settings->resamplerEnabled = c;
            m_settings->savePreferences();
            emit m_settings->settingsChanged();
            m_dspController->applyConfig();
        },
        [this, resItem]() { onSidebarItemClicked(resItem, 0); }, m_sidebarTree);
    m_sidebarTree->setItemWidget(resItem, 0, resW);

    for (size_t i = 0; i < m_pipeline->stages.size(); ++i) {
        const auto& stage = m_pipeline->stages[i];
        auto sItem = new QTreeWidgetItem(pipeGroup);
        QUuid stageId = stage.id;
        sItem->setData(0, Qt::UserRole, QString("stage_%1").arg(stageId.toString()));

        QString rawName = QString::fromStdString(stage.name);
        static const QStringList legacySymbols = {"speaker.wave.3",
                                                  "arrow.left.and.right",
                                                  "slider.vertical.3",
                                                  "slider.horizontal.3",
                                                  "waveform.path.ecg",
                                                  "waveform.path",
                                                  "headphones",
                                                  "arrow.left.and.right.circle",
                                                  "ear",
                                                  "waveform",
                                                  "bolt.shield",
                                                  "plus.minus",
                                                  "clock",
                                                  "square.slash",
                                                  "grid",
                                                  "arrow.up.right.and.arrow.down.left.rectangle",
                                                  "waveform.badge.minus",
                                                  "speaker.wave.2.bubble",
                                                  "square.grid.3x1.below.line.grid.1x2",
                                                  "function",
                                                  "arrow.up.and.down.and.arrow.left.and.right",
                                                  "scissors",
                                                  "dial.low"};
        for (const auto& sym : legacySymbols) {
            if (rawName.startsWith(sym)) {
                rawName = rawName.mid(sym.length()).trimmed();
                break;
            }
        }

        QString stageTitle = rawName;
        auto stageW = new SidebarToggleRowWidget(
            m_sidebarTree, sItem, stageTitle, stage.isEnabled,
            [this, stageId](bool c) {
                for (auto& st : m_pipeline->stages) {
                    if (st.id == stageId) {
                        st.isEnabled = c;
                        m_pipeline->save();
                        emit m_pipeline->pipelineChanged();
                        break;
                    }
                }
            },
            [this, sItem]() { onSidebarItemClicked(sItem, 0); }, m_sidebarTree);
        m_sidebarTree->setItemWidget(sItem, 0, stageW);
    }

    auto addStageItem = new QTreeWidgetItem(pipeGroup, {"Add Stage…"});
    addStageItem->setData(0, Qt::UserRole, "add_stage");
    addStageItem->setSizeHint(0, QSize(0, 26));

    // 4. Convolution Section
    auto convGroup = makeCategoryItem(m_sidebarTree, "Convolution");
    for (const auto& conv : m_pipeline->convPresets) {
        auto cItem = new QTreeWidgetItem(convGroup, {QString::fromStdString(conv.name)});
        cItem->setData(0, Qt::UserRole, QString("conv_%1").arg(conv.id.toString()));
        cItem->setSizeHint(0, QSize(0, 26));
    }
    auto impConvItem = new QTreeWidgetItem(convGroup, {"Import IR File(s)…"});
    impConvItem->setData(0, Qt::UserRole, "import_conv");
    impConvItem->setSizeHint(0, QSize(0, 26));

    auto roomItem = new QTreeWidgetItem(convGroup, {"Room Correction"});
    roomItem->setData(0, Qt::UserRole, "room_correction");
    roomItem->setSizeHint(0, QSize(0, 26));

    // 5. EQ Presets Section
    auto eqGroup = makeCategoryItem(m_sidebarTree, "EQ Presets");
    for (const auto& eq : m_pipeline->eqPresets) {
        auto eItem = new QTreeWidgetItem(eqGroup, {QString::fromStdString(eq.name)});
        eItem->setData(0, Qt::UserRole, QString("eq_%1").arg(eq.id.toString()));
        eItem->setSizeHint(0, QSize(0, 26));
    }
    auto addEqItem = new QTreeWidgetItem(eqGroup, {"Add EQ Preset"});
    addEqItem->setData(0, Qt::UserRole, "add_eq");
    addEqItem->setSizeHint(0, QSize(0, 26));
    auto oratoryItem = new QTreeWidgetItem(eqGroup, {"Oratory Presets"});
    oratoryItem->setData(0, Qt::UserRole, "oratory_eq");
    oratoryItem->setSizeHint(0, QSize(0, 26));
    auto autoEqItem = new QTreeWidgetItem(eqGroup, {"AutoEQ Presets"});
    autoEqItem->setData(0, Qt::UserRole, "auto_eq");
    autoEqItem->setSizeHint(0, QSize(0, 26));

    m_sidebarTree->blockSignals(false);
}

void MainWindow::showCentralWidget(QWidget* widget) {
    if (!widget)
        return;
    if (widget != m_unavailableWidget && m_unavailableWidget) {
        m_centralStack->removeWidget(m_unavailableWidget);
        m_unavailableWidget->deleteLater();
        m_unavailableWidget = nullptr;
    }
    if (m_centralStack->indexOf(widget) == -1) {
        m_centralStack->addWidget(widget);
    }
    m_centralStack->setCurrentWidget(widget);
}

void MainWindow::onSidebarItemClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item)
        return;

    QString tag = item->data(0, Qt::UserRole).toString();
    if (tag.isEmpty())
        return;

    if (tag == "add_stage") {
        QMenu menu(this);
        for (StageCategory cat :
             {StageCategory::Filters, StageCategory::Mixer, StageCategory::Processors, StageCategory::Others}) {
            QMenu* catMenu = menu.addMenu(QString::fromStdString(stageCategoryToString(cat)));
            for (StageType st : {StageType::Balance,
                                 StageType::Width,
                                 StageType::MSProc,
                                 StageType::PhaseInvert,
                                 StageType::Crossfeed,
                                 StageType::SplitWidth,
                                 StageType::EQ,
                                 StageType::GraphicEQ,
                                 StageType::Convolution,
                                 StageType::Loudness,
                                 StageType::Emphasis,
                                 StageType::DCProtection,
                                 StageType::Gain,
                                 StageType::Delay,
                                 StageType::LookaheadLimiter,
                                 StageType::LookaheadLimiterProc,
                                 StageType::Clipper,
                                 StageType::Volume,
                                 StageType::MatrixMixer,
                                 StageType::Compressor,
                                 StageType::NoiseGate,
                                 StageType::RACE,
                                 StageType::Dither,
                                 StageType::DiffEq,
                                 StageType::BiquadCombo}) {
                if (stageTypeToCategory(st) == cat) {
                    QAction* act = catMenu->addAction(QString::fromStdString(stageTypeToString(st)));
                    connect(act, &QAction::triggered, [this, st]() {
                        QUuid newId = m_pipeline->addStage(st);
                        m_lastActiveTag = QString("stage_%1").arg(newId.toString());
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
        AutoEqPickerDlg dlg(m_pipeline, m_dspController, this);
        dlg.exec();
        handleNavigationTag(m_lastActiveTag);
    } else if (tag == "oratory_eq") {
        OratoryPresetPickerDlg dlg(m_pipeline, m_dspController, this);
        dlg.exec();
        handleNavigationTag(m_lastActiveTag);
    } else if (tag == "import_conv") {
        ConvolutionImportDlg dlg(m_pipeline, this);
        dlg.exec();
        handleNavigationTag(m_lastActiveTag);
    } else if (tag == "room_correction") {
        RoomCorrectionDlg dlg(m_pipeline, this);
        dlg.exec();
        handleNavigationTag(m_lastActiveTag);
    } else {
        m_lastActiveTag = tag;
        handleNavigationTag(tag);
    }
}

void MainWindow::handleNavigationTag(const QString& tag) {
    m_lastActiveTag = tag;
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
        w = new DashboardView(m_monitoring, m_dspController, m_spectrumEngine, m_spectrogramEngine, m_vectorScopeEngine,
                              this);
    } else if (tag == "devices") {
        w = new DevicePickerView(m_devices, m_settings, this);
    } else if (tag == "levels") {
        w = new LevelMetersDetailView(m_monitoring, this);
    } else if (tag == "spectrum") {
        w = new SpectrumDetailView(m_spectrumEngine, m_devices, this);
    } else if (tag == "spectroscope") {
        w = new SpectrogramDetailView(m_spectrogramEngine, m_devices, this);
    } else if (tag == "vectorscope") {
        w = new VectorScopeDetailView(m_vectorScopeEngine, m_devices, this);
    } else if (tag == "analogVU") {
        w = new AnalogVUDetailView(m_monitoring, this);
    } else if (tag == "resampler") {
        w = new ResamplerDetailView(m_settings, m_devices, m_dspController, this);
    } else if (tag == "general_settings") {
        w = new GeneralSettingsView(m_settings, m_monitoring, this);
    } else if (tag == "logs") {
        w = new ConsoleLogsView(this);
    } else if (tag.startsWith("stage_")) {
        QUuid stageId = QUuid::fromString(tag.mid(6));
        for (const auto& stage : m_pipeline->stages) {
            if (stage.id == stageId) {
                w = new StageDetailView(stageId, m_pipeline, m_dspController, this);
                break;
            }
        }
    } else if (tag.startsWith("conv_")) {
        QUuid id = QUuid::fromString(tag.mid(5));
        for (const auto& preset : m_pipeline->convPresets) {
            if (preset.id == id) {
                w = new ConvolutionPresetDetailView(preset, m_pipeline, m_devices, this);
                break;
            }
        }
    } else if (tag.startsWith("eq_")) {
        QUuid id = QUuid::fromString(tag.mid(3));
        for (const auto& preset : m_pipeline->eqPresets) {
            if (preset.id == id) {
                auto eqView = new EQPresetDetailView(preset, m_pipeline, m_dspController, this);
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
    // 1. Check if m_lastActiveTag is still valid
    bool tagStillValid = false;
    if (m_lastActiveTag.startsWith("stage_")) {
        QUuid stageId = QUuid::fromString(m_lastActiveTag.mid(6));
        for (const auto& st : m_pipeline->stages) {
            if (st.id == stageId) {
                tagStillValid = true;
                break;
            }
        }
    } else if (m_lastActiveTag.startsWith("eq_")) {
        QUuid id = QUuid::fromString(m_lastActiveTag.mid(3));
        for (const auto& eq : m_pipeline->eqPresets) {
            if (eq.id == id) {
                tagStillValid = true;
                break;
            }
        }
    } else if (m_lastActiveTag.startsWith("conv_")) {
        QUuid id = QUuid::fromString(m_lastActiveTag.mid(5));
        for (const auto& conv : m_pipeline->convPresets) {
            if (conv.id == id) {
                tagStillValid = true;
                break;
            }
        }
    } else {
        tagStillValid = true;
    }

    // 2. Destroy cached detail views of dynamic pipeline components EXCEPT the currently active view
    QList<QString> keysToDestroy;
    for (auto it = m_pageCache.begin(); it != m_pageCache.end(); ++it) {
        QString k = it.key();
        if (k.startsWith("stage_") || k.startsWith("eq_") || k.startsWith("conv_")) {
            if (!tagStillValid || k != m_lastActiveTag) {
                keysToDestroy.append(k);
            }
        }
    }
    for (const auto& key : keysToDestroy) {
        QWidget* w = m_pageCache.take(key);
        if (w) {
            m_centralStack->removeWidget(w);
            w->deleteLater();
        }
    }

    // 3. Refresh sidebar items
    refreshSidebarItems();
    updateTrayMenu();
    updateStatusBar();

    // 4. Retain current active view if valid; show placeholder if deleted
    if (tagStillValid) {
        if (!m_pageCache.contains(m_lastActiveTag) || !m_pageCache[m_lastActiveTag]) {
            handleNavigationTag(m_lastActiveTag);
        }
    } else {
        if (m_unavailableWidget) {
            m_centralStack->removeWidget(m_unavailableWidget);
            m_unavailableWidget->deleteLater();
        }
        m_unavailableWidget = new QWidget(this);
        auto layout = new QVBoxLayout(m_unavailableWidget);
        auto lblTitle = new QLabel("Item Deleted", m_unavailableWidget);
        {
            QFont font = lblTitle->font();
            font.setPointSize(18);
            font.setBold(true);
            lblTitle->setFont(font);
        }
        lblTitle->setAlignment(Qt::AlignCenter);
        auto lblDesc = new QLabel("Select another stage or preset from the sidebar.", m_unavailableWidget);
        {
            QFont font = lblDesc->font();
            font.setPointSize(13);
            lblDesc->setFont(font);
        }
        lblDesc->setAlignment(Qt::AlignCenter);
        layout->addStretch();
        layout->addWidget(lblTitle);
        layout->addWidget(lblDesc);
        layout->addStretch();
        showCentralWidget(m_unavailableWidget);
    }
}

void MainWindow::showAndActivate() {
    MacUtils::showDockIcon();
    showNormal();
    raise();
    activateWindow();
}
