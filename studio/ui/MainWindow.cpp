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
#include "ui/StyleTheme.h"
#include "ui/VectorScopeView.h"
#include "ui/VisualizerDetailViews.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>

namespace {
class SidebarToggleRowWidget : public QWidget {
public:
    SidebarToggleRowWidget(QTreeWidget* tree, QTreeWidgetItem* item, const QString& title, bool isChecked,
                           std::function<void(bool)> onToggle, std::function<void()> onRowClick,
                           QWidget* parent = nullptr)
        : QWidget(parent), m_tree(tree), m_item(item), m_onToggle(onToggle), m_onRowClick(onRowClick) {
        setAutoFillBackground(false);
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet("QWidget { background: transparent; }");

        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 2, 8, 2);
        layout->setSpacing(6);

        m_label = new QLabel(title, this);
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_label->setStyleSheet("QLabel { background: transparent; }");
        layout->addWidget(m_label);

        layout->addStretch();

        m_checkbox = new QCheckBox(this);
        m_checkbox->setFocusPolicy(Qt::NoFocus);
        m_checkbox->setChecked(isChecked);
        m_checkbox->setStyleSheet(
            "QCheckBox { background: transparent; } QCheckBox::indicator { width: 14px; height: 14px; }");
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
    connect(m_miniPlayer.get(), &MiniPlayerView::requestRestoreMainWindow, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });

    resize(1100, 780);
    setMinimumSize(960, 680);
    setWindowTitle("DSP Monitor");

    setupUi();
    setupMenuBar();
    setupStatusBar();
    setupTrayIcon();
    setupShortcuts();
    updateTheme();

    // Wire app state callbacks
    m_settings->onChanged = [this]() {
        m_devices->validateSampleRates();
        m_dspController->applyConfig();
    };

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
                } else if (state == ProcessingState::Running && m_stopReasonBanner) {
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
        static bool lastDarkMode = m_settings->darkMode;
        updateMuteDisplay();
        updateVolumeDisplay();
        updateStatusBar();
        if (m_settings->darkMode != lastDarkMode) {
            lastDarkMode = m_settings->darkMode;
            updateTheme();
        }
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
}

void MainWindow::updateTheme() {
    StyleTheme::applyTheme(qApp, m_settings->darkMode ? AppTheme::Dark : AppTheme::Light);
    if (m_actThemeLight && m_actThemeDark) {
        m_actThemeLight->blockSignals(true);
        m_actThemeDark->blockSignals(true);
        m_actThemeLight->setChecked(!m_settings->darkMode);
        m_actThemeDark->setChecked(m_settings->darkMode);
        m_actThemeLight->blockSignals(false);
        m_actThemeDark->blockSignals(false);
    }
    for (QWidget* w : qApp->allWidgets()) {
        w->update();
    }
}

void MainWindow::setupUi() {
    setupToolbar();

    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);

    m_sidebarTree = new QTreeWidget(m_splitter);
    m_sidebarTree->setHeaderHidden(true);
    m_sidebarTree->setMinimumWidth(220);
    m_sidebarTree->setMaximumWidth(360);
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
    m_splitter->setSizes({260, 840});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, false);

    setupSidebar();
}

void MainWindow::setupStatusBar() {
    auto bar = statusBar();
    m_statusStateLabel = new QLabel("🔴 Inactive", this);
    m_statusSampleRateBadge = new QLabel("48000 Hz", this);
    m_statusBufferLabel = new QLabel("Buffer: 1024", this);
    m_statusResamplerLabel = new QLabel("Resampler: Off", this);
    m_statusStagesLabel = new QLabel("Stages: 0 active", this);
    m_statusActivePresetLabel = new QLabel("Preset: None", this);
    m_statusRuntimeLabel = new QLabel("Run Time: 00:00:00", this);
    m_stopReasonBanner = new QLabel(this);
    m_statusMuteLabel = new QLabel("Unmuted", this);

    m_statusStateLabel->setStyleSheet("padding: 0 8px; color: #ff3b30; font-weight: bold;");
    m_statusSampleRateBadge->setStyleSheet("padding: 2px 8px; color: #007aff; background-color: rgba(0, 122, 255, "
                                           "0.15); border-radius: 4px; font-weight: bold; font-family: monospace;");
    m_statusBufferLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusResamplerLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusStagesLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusActivePresetLabel->setStyleSheet("padding: 0 8px; color: #8e8e93;");
    m_statusRuntimeLabel->setStyleSheet("padding: 0 8px; color: #8e8e93; font-family: monospace;");
    m_stopReasonBanner->setStyleSheet(
        "padding: 2px 8px; color: #ffffff; background-color: #ff3b30; border-radius: 4px; font-weight: bold;");
    m_stopReasonBanner->hide();
    m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #34c759; font-weight: bold;");

    bar->addWidget(m_statusStateLabel);
    bar->addWidget(m_statusSampleRateBadge);
    bar->addWidget(m_statusBufferLabel);
    bar->addWidget(m_statusResamplerLabel);
    bar->addWidget(m_statusStagesLabel);
    bar->addWidget(m_statusActivePresetLabel);
    bar->addWidget(m_statusRuntimeLabel);
    bar->addWidget(m_stopReasonBanner);
    bar->addPermanentWidget(m_statusMuteLabel);

    connect(&m_runtimeUpdateTimer, &QTimer::timeout, this, [this]() {
        if (m_dspController && m_dspController->status == ProcessingState::Running) {
            qint64 secs = m_engineRunTimer.elapsed() / 1000;
            int h = static_cast<int>(secs / 3600);
            int m = static_cast<int>((secs % 3600) / 60);
            int s = static_cast<int>(secs % 60);
            m_statusRuntimeLabel->setText(QString("Run Time: %1:%2:%3")
                                              .arg(h, 2, 10, QChar('0'))
                                              .arg(m, 2, 10, QChar('0'))
                                              .arg(s, 2, 10, QChar('0')));
        } else {
            m_statusRuntimeLabel->setText("Run Time: 00:00:00");
        }
    });
    m_runtimeUpdateTimer.setInterval(1000);
    updateStatusBar();
}

void MainWindow::setupMenuBar() {
    auto bar = menuBar();

    // 1. File Menu
    auto fileMenu = bar->addMenu("&File");

    m_actAddEqPreset = new QAction("New EQ Preset", this);
    m_actAddEqPreset->setShortcuts({QKeySequence("Cmd+N"), QKeySequence("Ctrl+N")});
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
    m_actImportConv->setShortcuts({QKeySequence("Cmd+O"), QKeySequence("Ctrl+O")});
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

    auto closeAct = new QAction("Close Window", this);
    closeAct->setShortcuts({QKeySequence("Cmd+W"), QKeySequence("Ctrl+W")});
    connect(closeAct, &QAction::triggered, this, &QMainWindow::close);
    fileMenu->addAction(closeAct);

    fileMenu->addSeparator();

    auto settingsAct = new QAction("Settings...", this);
    settingsAct->setMenuRole(QAction::PreferencesRole);
    settingsAct->setShortcuts({QKeySequence("Cmd+,"), QKeySequence("Ctrl+,")});
    connect(settingsAct, &QAction::triggered, [this]() { handleNavigationTag("general_settings"); });
    fileMenu->addAction(settingsAct);

    auto aboutAct = new QAction("About CamillaDSP Monitor", this);
    aboutAct->setMenuRole(QAction::AboutRole);
    connect(aboutAct, &QAction::triggered, [this]() {
        QMessageBox::about(this, "About CamillaDSP Monitor",
                           "CamillaDSP Monitor\n\nA macOS / Qt audio DSP monitoring and pipeline controller.");
    });
    fileMenu->addAction(aboutAct);

    auto quitAct = new QAction("Quit CamillaDSP Monitor", this);
    quitAct->setShortcuts({QKeySequence("Cmd+Q"), QKeySequence("Ctrl+Q")});
    quitAct->setMenuRole(QAction::QuitRole);
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);
    fileMenu->addAction(quitAct);

    // 2. View Menu
    auto viewMenu = bar->addMenu("&View");
    auto setupViewAct = [this, viewMenu](const QString& title, const QList<QKeySequence>& seqs, const QString& tag) {
        auto act = new QAction(title, this);
        act->setShortcuts(seqs);
        connect(act, &QAction::triggered, [this, tag]() { handleNavigationTag(tag); });
        viewMenu->addAction(act);
    };

    setupViewAct("Devices", {QKeySequence("Cmd+1"), QKeySequence("Ctrl+1")}, "devices");
    setupViewAct("Dashboard", {QKeySequence("Cmd+2"), QKeySequence("Ctrl+2")}, "dashboard");
    setupViewAct("Level Meters", {QKeySequence("Cmd+3"), QKeySequence("Ctrl+3")}, "levels");
    setupViewAct("Spectrum", {QKeySequence("Cmd+4"), QKeySequence("Ctrl+4")}, "spectrum");
    setupViewAct("Spectroscope Waterfall", {QKeySequence("Cmd+5"), QKeySequence("Ctrl+5")}, "spectroscope");
    setupViewAct("Vector Scope", {QKeySequence("Cmd+6"), QKeySequence("Ctrl+6")}, "vectorscope");
    setupViewAct("Analog VU Meter", {QKeySequence("Cmd+7"), QKeySequence("Ctrl+7")}, "analogVU");
    setupViewAct("Console Logs", {QKeySequence("Cmd+8"), QKeySequence("Ctrl+8")}, "logs");
    setupViewAct("General Settings", {QKeySequence("Cmd+9"), QKeySequence("Ctrl+9")}, "general_settings");

    viewMenu->addSeparator();

    // Theme Submenu under View
    auto themeSubMenu = viewMenu->addMenu("Theme");
    auto themeGroup = new QActionGroup(this);

    m_actThemeLight = new QAction("Light Mode", this);
    m_actThemeLight->setCheckable(true);
    m_actThemeLight->setChecked(!m_settings->darkMode);
    connect(m_actThemeLight, &QAction::triggered, [this]() {
        if (m_settings->darkMode) {
            m_settings->darkMode = false;
            m_settings->savePreferences();
            emit m_settings->settingsChanged();
        }
    });
    themeGroup->addAction(m_actThemeLight);
    themeSubMenu->addAction(m_actThemeLight);

    m_actThemeDark = new QAction("Dark Mode", this);
    m_actThemeDark->setCheckable(true);
    m_actThemeDark->setChecked(m_settings->darkMode);
    connect(m_actThemeDark, &QAction::triggered, [this]() {
        if (!m_settings->darkMode) {
            m_settings->darkMode = true;
            m_settings->savePreferences();
            emit m_settings->settingsChanged();
        }
    });
    themeGroup->addAction(m_actThemeDark);
    themeSubMenu->addAction(m_actThemeDark);

    viewMenu->addSeparator();

    auto miniAct = new QAction("Toggle MiniPlayer", this);
    miniAct->setShortcuts({QKeySequence("Cmd+Shift+M"), QKeySequence("Ctrl+Shift+M")});
    connect(miniAct, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);
    viewMenu->addAction(miniAct);

    // 3. Audio Menu
    auto audioMenu = bar->addMenu("&Audio");
    auto startStopAct = new QAction("Start/Stop Engine", this);
    startStopAct->setShortcut(QKeySequence(Qt::Key_Space));
    connect(startStopAct, &QAction::triggered, [this]() {
        if (m_dspController->status == ProcessingState::Running) {
            m_dspController->stopEngine();
        } else {
            m_dspController->startEngine();
        }
    });
    audioMenu->addAction(startStopAct);

    auto muteAct = new QAction("Toggle Mute", this);
    muteAct->setShortcut(QKeySequence(Qt::Key_M));
    connect(muteAct, &QAction::triggered, [this]() {
        auto focusW = QApplication::focusWidget();
        for (QWidget* w = focusW; w; w = w->parentWidget()) {
            if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QTextEdit*>(w) ||
                qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w)) {
                return;
            }
        }
        toggleMute();
    });
    audioMenu->addAction(muteAct);

    // 4. Window Menu
    auto windowMenu = bar->addMenu("&Window");
    auto minAct = new QAction("Minimize", this);
    minAct->setShortcuts({QKeySequence("Cmd+M"), QKeySequence("Ctrl+M")});
    connect(minAct, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);
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
    connect(bringAllAct, &QAction::triggered, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    windowMenu->addAction(bringAllAct);

    // 5. Help Menu
    auto helpMenu = bar->addMenu("&Help");
    auto helpAct = new QAction("CamillaDSP Monitor Help", this);
    connect(helpAct, &QAction::triggered,
            [this]() { QDesktopServices::openUrl(QUrl("https://github.com/HSAudio/CamillaDSP")); });
    helpMenu->addAction(helpAct);
}

void MainWindow::setupTrayIcon() {
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QIcon::fromTheme("audio-card", QIcon(":/icons/app.png")));

    m_trayMenu = new QMenu(this);

    auto showAct = m_trayMenu->addAction("Show Main Window");
    connect(showAct, &QAction::triggered, this, [this]() {
        if (m_miniPlayer && m_miniPlayer->isVisible()) {
            m_miniPlayer->hide();
        }
        showNormal();
        raise();
        activateWindow();
    });

    auto miniPlayerAct = m_trayMenu->addAction("Toggle MiniPlayer");
    connect(miniPlayerAct, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);

    m_trayMenu->addSeparator();

    m_trayMuteAction = m_trayMenu->addAction("Mute Audio");
    m_trayMuteAction->setCheckable(true);
    connect(m_trayMuteAction, &QAction::triggered, this, &MainWindow::toggleMute);

    m_trayPresetSubMenu = m_trayMenu->addMenu("Active EQ Preset");

    m_trayMenu->addSeparator();

    auto quitAct = m_trayMenu->addAction("Quit CamillaDSP Monitor");
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->show();

    updateTrayMenu();
}

void MainWindow::updateTrayMenu() {
    if (!m_trayMuteAction || !m_trayPresetSubMenu)
        return;

    bool muted = m_settings->getMuted(Fader::Main);
    m_trayMuteAction->setChecked(muted);
    m_trayMuteAction->setText(muted ? "🔊 Unmute Audio" : "🔇 Mute Audio");

    m_trayPresetSubMenu->clear();

    QUuid activeId;
    for (const auto& stage : m_pipeline->stages) {
        if (stage.type == StageType::EQ && stage.eqPresetId.has_value()) {
            activeId = stage.eqPresetId.value();
            break;
        }
    }
    if (activeId.isNull() && !m_pipeline->eqPresets.empty()) {
        activeId = m_pipeline->eqPresets.front().id;
    }

    if (m_pipeline->eqPresets.empty()) {
        auto emptyAct = m_trayPresetSubMenu->addAction("No EQ Presets Available");
        emptyAct->setEnabled(false);
    } else {
        for (const auto& preset : m_pipeline->eqPresets) {
            auto act = m_trayPresetSubMenu->addAction(QString::fromStdString(preset.name));
            act->setCheckable(true);
            bool isActive = (preset.id == activeId);
            act->setChecked(isActive);

            QUuid id = preset.id;
            connect(act, &QAction::triggered, [this, id]() { selectActiveEQPreset(id); });
        }
    }
}

void MainWindow::selectActiveEQPreset(const QUuid& presetId) {
    bool foundEqStage = false;
    for (auto& stage : m_pipeline->stages) {
        if (stage.type == StageType::EQ) {
            stage.eqPresetId = presetId;
            foundEqStage = true;
        }
    }
    if (!foundEqStage) {
        PipelineStage newStage;
        newStage.id = QUuid::createUuid();
        newStage.name = "Equalizer";
        newStage.type = StageType::EQ;
        newStage.isEnabled = true;
        newStage.eqPresetId = presetId;
        m_pipeline->stages.push_back(newStage);
    }
    m_pipeline->save();
    m_dspController->applyConfig();
    updateStatusBar();
    updateTrayMenu();
}

void MainWindow::setupShortcuts() {
    auto setupNavAction = [this](const QList<QKeySequence>& seqs, const QString& tag) {
        auto act = new QAction(this);
        act->setShortcuts(seqs);
        connect(act, &QAction::triggered, [this, tag]() { handleNavigationTag(tag); });
        addAction(act);
    };

    setupNavAction({QKeySequence("Cmd+1"), QKeySequence("Ctrl+1")}, "devices");
    setupNavAction({QKeySequence("Cmd+2"), QKeySequence("Ctrl+2")}, "dashboard");
    setupNavAction({QKeySequence("Cmd+3"), QKeySequence("Ctrl+3")}, "levels");
    setupNavAction({QKeySequence("Cmd+4"), QKeySequence("Ctrl+4")}, "spectrum");
    setupNavAction({QKeySequence("Cmd+5"), QKeySequence("Ctrl+5")}, "spectroscope");
    setupNavAction({QKeySequence("Cmd+6"), QKeySequence("Ctrl+6")}, "vectorscope");
    setupNavAction({QKeySequence("Cmd+7"), QKeySequence("Ctrl+7")}, "analogVU");
    setupNavAction({QKeySequence("Cmd+8"), QKeySequence("Ctrl+8")}, "logs");
    setupNavAction({QKeySequence("Cmd+9"), QKeySequence("Ctrl+9"), QKeySequence("Cmd+,"), QKeySequence("Ctrl+,")},
                   "general_settings");

    // Cmd+M / Ctrl+M: MiniPlayer Toggle
    auto actMini = new QAction(this);
    actMini->setShortcuts({QKeySequence("Cmd+M"), QKeySequence("Ctrl+M")});
    connect(actMini, &QAction::triggered, this, &MainWindow::toggleMiniPlayer);
    addAction(actMini);

    // Cmd+W / Ctrl+W: Close / Hide Window
    auto actClose = new QAction(this);
    actClose->setShortcuts({QKeySequence("Cmd+W"), QKeySequence("Ctrl+W")});
    connect(actClose, &QAction::triggered, this, &QMainWindow::close);
    addAction(actClose);

    if (m_actImportConv)
        addAction(m_actImportConv);
    if (m_actAddEqPreset)
        addAction(m_actAddEqPreset);
    if (m_actRoomCorrection)
        addAction(m_actRoomCorrection);
    if (m_actOratoryPreset)
        addAction(m_actOratoryPreset);
    if (m_actAutoEqPreset)
        addAction(m_actAutoEqPreset);

    // Space: Start/Stop Engine (Pause/Resume)
    auto actStartStop = new QAction(this);
    actStartStop->setShortcut(QKeySequence("Space"));
    connect(actStartStop, &QAction::triggered, [this]() {
        auto focusW = QApplication::focusWidget();
        for (QWidget* w = focusW; w; w = w->parentWidget()) {
            if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QTextEdit*>(w) ||
                qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w)) {
                return;
            }
        }
        if (m_dspController->status == ProcessingState::Running) {
            m_dspController->stopEngine();
        } else {
            m_dspController->startEngine();
        }
    });
    addAction(actStartStop);

    // M: Toggle Mute
    auto actMute = new QAction(this);
    actMute->setShortcut(QKeySequence(Qt::Key_M));
    connect(actMute, &QAction::triggered, [this]() {
        auto focusW = QApplication::focusWidget();
        for (QWidget* w = focusW; w; w = w->parentWidget()) {
            if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QTextEdit*>(w) ||
                qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w)) {
                return;
            }
        }
        toggleMute();
    });
    addAction(actMute);

    // Esc Key
    auto actEsc = new QAction(this);
    actEsc->setShortcut(QKeySequence("Esc"));
    connect(actEsc, &QAction::triggered, [this]() {
        if (m_miniPlayer && m_miniPlayer->isVisible()) {
            m_miniPlayer->hide();
            showNormal();
            raise();
            activateWindow();
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

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (isMinimized()) {
            setWindowState(windowState() & ~Qt::WindowMinimized);
            toggleMiniPlayer();
        }
    } else if (event->type() == QEvent::ApplicationActivate) {
        if (m_miniPlayer && m_miniPlayer->isVisible()) {
            m_miniPlayer->hide();
            showNormal();
            raise();
            activateWindow();
        }
    }
}

void MainWindow::toggleMute() {
    bool currentMute = m_settings->getMuted(Fader::Main);
    m_dspController->setFaderMute(Fader::Main, !currentMute);
    updateMuteDisplay();
}

void MainWindow::updateMuteDisplay() {
    bool muted = m_settings->getMuted(Fader::Main);
    if (muted) {
        m_toolbarMuteBtn->setText("🔇");
        m_toolbarMuteBtn->setStyleSheet(
            "QPushButton { background: transparent; color: #ff3b30; font-size: 14px; padding: 2px 4px; border: none; } "
            "QPushButton:hover { background-color: rgba(255, 59, 48, 0.1); border-radius: 4px; }");
        m_statusMuteLabel->setText("MUTED");
        m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #ff3b30; font-weight: bold;");
    } else {
        m_toolbarMuteBtn->setText("🔊");
        m_toolbarMuteBtn->setStyleSheet(
            "QPushButton { background: transparent; color: inherit; font-size: 14px; padding: 2px 4px; border: none; } "
            "QPushButton:hover { background-color: rgba(255, 255, 255, 0.1); border-radius: 4px; }");
        m_statusMuteLabel->setText("Unmuted");
        m_statusMuteLabel->setStyleSheet("padding: 0 8px; color: #34c759; font-weight: bold;");
    }

    if (m_trayMuteAction) {
        m_trayMuteAction->setChecked(muted);
        m_trayMuteAction->setText(muted ? "🔊 Unmute Audio" : "🔇 Mute Audio");
    }
}

void MainWindow::updateVolumeDisplay() {
    float gain = m_settings->getVolume(Fader::Main);
    m_headerVolumeSlider->blockSignals(true);
    m_headerVolumeSlider->setValue(static_cast<int>(std::round(gain * 2.0f)));
    m_headerVolumeSlider->blockSignals(false);

    m_gainValueLabel->setText(QString::asprintf("%+.1f dB", gain));
    if (gain > 0.0f) {
        m_gainValueLabel->setStyleSheet(
            "font-family: monospace; font-size: 10pt; color: #ff3b30; padding-right: 10px;");
    } else {
        m_gainValueLabel->setStyleSheet(
            QString("font-family: monospace; font-size: 10pt; color: %1; padding-right: 10px;")
                .arg(StyleTheme::textPrimary().name()));
    }
}

void MainWindow::updateStatusBar() {
    QString stateStr;
    QString colorStyle;
    switch (m_dspController->status) {
    case ProcessingState::Running:
        stateStr = "🟢 Running";
        colorStyle = "padding: 0 8px; color: #34c759; font-weight: bold;";
        break;
    case ProcessingState::Starting:
        stateStr = "🟡 Starting";
        colorStyle = "padding: 0 8px; color: #ffcc00; font-weight: bold;";
        break;
    case ProcessingState::Paused:
        stateStr = "🟡 Paused";
        colorStyle = "padding: 0 8px; color: #ffcc00; font-weight: bold;";
        break;
    case ProcessingState::Stalled:
        stateStr = "🟡 Stalled";
        colorStyle = "padding: 0 8px; color: #ffcc00; font-weight: bold;";
        break;
    case ProcessingState::Inactive:
    default:
        stateStr = "🔴 Inactive";
        colorStyle = "padding: 0 8px; color: #ff3b30; font-weight: bold;";
        break;
    }
    m_statusStateLabel->setText(QString("State: %1").arg(stateStr));
    m_statusStateLabel->setStyleSheet(colorStyle);
    m_statusBufferLabel->setText(QString("Buffer: %1").arg(m_settings->chunkSize));
    if (m_statusSampleRateBadge) {
        m_statusSampleRateBadge->setText(QString("%1 Hz").arg(m_devices->captureConfig.sampleRate));
    }

    QUuid activeId;
    for (const auto& stage : m_pipeline->stages) {
        if (stage.type == StageType::EQ && stage.eqPresetId.has_value()) {
            activeId = stage.eqPresetId.value();
            break;
        }
    }
    QString presetName = "None";
    if (!activeId.isNull()) {
        for (const auto& eq : m_pipeline->eqPresets) {
            if (eq.id == activeId) {
                presetName = QString::fromStdString(eq.name);
                break;
            }
        }
    }
    m_statusActivePresetLabel->setText(QString("Preset: %1").arg(presetName));
    if (m_statusResamplerLabel) {
        m_statusResamplerLabel->setText(m_settings->resamplerEnabled ? "Resampler: On" : "Resampler: Off");
    }
    if (m_statusStagesLabel) {
        int activeCount = 0;
        for (const auto& stage : m_pipeline->stages) {
            if (stage.isEnabled) {
                activeCount++;
            }
        }
        m_statusStagesLabel->setText(QString("Stages: %1 active").arg(activeCount));
    }
    updateMuteDisplay();
}

void MainWindow::onEngineStatusChanged(ProcessingState state) {
    switch (state) {
    case ProcessingState::Running:
    case ProcessingState::Paused:
    case ProcessingState::Stalled:
        m_startStopBtn->setText("🛑 Stop");
        m_startStopBtn->setToolTip("Stop Engine");
        m_startStopBtn->setStyleSheet(
            "QPushButton { background-color: transparent; color: #ff3b30; font-weight: bold; padding: 4px 8px; border: "
            "none; } "
            "QPushButton:hover { background-color: rgba(255, 59, 48, 0.1); border-radius: 4px; }");
        if (state == ProcessingState::Running) {
            if (!m_engineRunTimer.isValid() || m_engineRunTimer.elapsed() == 0) {
                m_engineRunTimer.start();
            }
            m_runtimeUpdateTimer.start();
        } else {
            m_runtimeUpdateTimer.stop();
        }
        break;
    case ProcessingState::Starting:
        m_startStopBtn->setText("⏳ Starting...");
        m_startStopBtn->setToolTip("Starting Engine...");
        m_startStopBtn->setStyleSheet("QPushButton { background-color: transparent; color: #ffcc00; font-weight: bold; "
                                      "padding: 4px 8px; border: none; }");
        break;
    case ProcessingState::Inactive:
    default:
        m_startStopBtn->setText("▶️ Start");
        m_startStopBtn->setToolTip("Start Engine");
        m_startStopBtn->setStyleSheet(
            "QPushButton { background-color: transparent; color: #34c759; font-weight: bold; padding: 4px 8px; border: "
            "none; } "
            "QPushButton:hover { background-color: rgba(52, 199, 89, 0.1); border-radius: 4px; }");
        m_runtimeUpdateTimer.stop();
        if (m_statusRuntimeLabel) {
            m_statusRuntimeLabel->setText("Run Time: 00:00:00");
        }
        m_engineRunTimer.invalidate();
        break;
    }
    if (m_sampleRateBadge) {
        m_sampleRateBadge->setText(QString("%1 Hz").arg(m_devices->captureConfig.sampleRate));
    }
    updateStatusBar();
}

void MainWindow::setupToolbar() {
    auto toolBar = addToolBar("Main Controls");
    toolBar->setMovable(false);

    m_startStopBtn = new QPushButton("▶️ Start", this);
    m_startStopBtn->setToolTip("Start Engine");
    m_startStopBtn->setStyleSheet(
        "QPushButton { background-color: transparent; color: #34c759; font-weight: bold; padding: 4px 8px; border: "
        "none; } "
        "QPushButton:hover { background-color: rgba(52, 199, 89, 0.1); border-radius: 4px; }");
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
    m_sampleRateBadge->setFont(QFont("monospace", 11));
    m_sampleRateBadge->setStyleSheet("color: #8e8e93; padding-right: 8px; padding-left: 8px;");
    toolBar->addWidget(m_sampleRateBadge);

    m_toolbarMuteBtn = new QPushButton("🔊", this);
    m_toolbarMuteBtn->setToolTip("Toggle Mute");
    m_toolbarMuteBtn->setFlat(true);
    m_toolbarMuteBtn->setStyleSheet(
        "QPushButton { background: transparent; font-size: 14px; padding: 2px 4px; border: none; } "
        "QPushButton:hover { background-color: rgba(255, 255, 255, 0.1); border-radius: 4px; }");
    connect(m_toolbarMuteBtn, &QPushButton::clicked, this, &MainWindow::toggleMute);
    toolBar->addWidget(m_toolbarMuteBtn);

    // 400px Volume slider (-60 to +20 dB in 0.5 dB steps: mapped to -120 to +40 int range)
    m_headerVolumeSlider = new QSlider(Qt::Horizontal, this);
    m_headerVolumeSlider->setRange(-120, 40);
    m_headerVolumeSlider->setSingleStep(1);
    m_headerVolumeSlider->setPageStep(2);
    m_headerVolumeSlider->setValue(static_cast<int>(m_settings->getVolume(Fader::Main) * 2.0f));
    m_headerVolumeSlider->setFixedWidth(400);
    connect(m_headerVolumeSlider, &QSlider::valueChanged, [this](int val) {
        float db = val / 2.0f;
        m_dspController->setFaderVolume(Fader::Main, db);
        updateVolumeDisplay();
    });
    toolBar->addWidget(m_headerVolumeSlider);

    m_gainValueLabel = new QLabel("  0.0 dB", this);
    m_gainValueLabel->setFont(QFont("monospace", 10));
    m_gainValueLabel->setFixedWidth(50);
    m_gainValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_gainValueLabel->setStyleSheet("padding-right: 10px;");
    updateVolumeDisplay();
    toolBar->addWidget(m_gainValueLabel);
}

void MainWindow::toggleMiniPlayer() {
    if (m_miniPlayer->isVisible()) {
        m_miniPlayer->hide();
        showNormal();
        raise();
        activateWindow();
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
        m_sidebarTree, spectroItem, "Spectroscope", m_settings->showSpectrogramInDashboard,
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

    // 3. Pipeline Section
    auto pipeGroup = new QTreeWidgetItem(m_sidebarTree, {"Pipeline"});
    pipeGroup->setExpanded(true);

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

    // 4. Convolution Section
    auto convGroup = new QTreeWidgetItem(m_sidebarTree, {"Convolution"});
    convGroup->setExpanded(true);
    for (const auto& conv : m_pipeline->convPresets) {
        auto cItem = new QTreeWidgetItem(convGroup, {QString::fromStdString(conv.name)});
        cItem->setData(0, Qt::UserRole, QString("conv_%1").arg(conv.id.toString()));
    }
    auto impConvItem = new QTreeWidgetItem(convGroup, {"Import IR File(s)…"});
    impConvItem->setData(0, Qt::UserRole, "import_conv");

    auto roomItem = new QTreeWidgetItem(convGroup, {"Room Correction"});
    roomItem->setData(0, Qt::UserRole, "room_correction");

    // 5. EQ Presets Section
    auto eqGroup = new QTreeWidgetItem(m_sidebarTree, {"EQ Presets"});
    eqGroup->setExpanded(true);
    for (const auto& eq : m_pipeline->eqPresets) {
        auto eItem = new QTreeWidgetItem(eqGroup, {QString::fromStdString(eq.name)});
        eItem->setData(0, Qt::UserRole, QString("eq_%1").arg(eq.id.toString()));
    }
    auto addEqItem = new QTreeWidgetItem(eqGroup, {"Add"});
    addEqItem->setData(0, Qt::UserRole, "add_eq");
    auto oratoryItem = new QTreeWidgetItem(eqGroup, {"Oratory"});
    oratoryItem->setData(0, Qt::UserRole, "oratory_eq");
    auto autoEqItem = new QTreeWidgetItem(eqGroup, {"AutoEQ"});
    autoEqItem->setData(0, Qt::UserRole, "auto_eq");

    m_sidebarTree->blockSignals(false);
}

void MainWindow::showCentralWidget(QWidget* widget) {
    if (!widget)
        return;
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
            for (StageType st : {StageType::Balance,     StageType::Width,     StageType::MSProc,
                                 StageType::PhaseInvert, StageType::Crossfeed, StageType::SplitWidth,
                                 StageType::EQ,          StageType::GraphicEQ, StageType::Convolution,
                                 StageType::Loudness,    StageType::Emphasis,  StageType::DCProtection,
                                 StageType::Gain,        StageType::Delay,     StageType::LookaheadLimiter,
                                 StageType::Clipper,     StageType::Volume,    StageType::MatrixMixer,
                                 StageType::Compressor,  StageType::NoiseGate, StageType::RACE,
                                 StageType::Dither,      StageType::DiffEq,    StageType::BiquadCombo}) {
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
        auto container = new QWidget(this);
        auto layout = new QVBoxLayout(container);
        layout->setContentsMargins(16, 16, 16, 16);
        auto cap = new LevelMeterView(container);
        cap->setLevelState(&m_monitoring->levelState);
        cap->setIsCapture(true);
        auto pb = new LevelMeterView(container);
        pb->setLevelState(&m_monitoring->levelState);
        pb->setIsCapture(false);
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
        for (size_t idx = 0; idx < m_pipeline->stages.size(); ++idx) {
            if (m_pipeline->stages[idx].id == stageId) {
                w = new StageDetailView(idx, m_pipeline, m_dspController, this);
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

    // 2. Destroy cached detail views EXCEPT active tag if still valid
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

    // 4. Retain current active view if valid; show placeholder if deleted
    if (tagStillValid) {
        handleNavigationTag(m_lastActiveTag);
    } else {
        auto unavailable = new QWidget(this);
        auto layout = new QVBoxLayout(unavailable);
        auto lblTitle = new QLabel("Item Deleted", unavailable);
        lblTitle->setFont(QFont("-apple-system", 18, QFont::Bold));
        lblTitle->setAlignment(Qt::AlignCenter);
        auto lblDesc = new QLabel("Select another stage or preset from the sidebar.", unavailable);
        lblDesc->setStyleSheet("color: #8e8e93; font-size: 13px;");
        lblDesc->setAlignment(Qt::AlignCenter);
        layout->addStretch();
        layout->addWidget(lblTitle);
        layout->addWidget(lblDesc);
        layout->addStretch();
        showCentralWidget(unavailable);
    }
}
