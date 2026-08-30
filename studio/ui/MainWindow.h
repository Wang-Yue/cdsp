#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "config/DSPConfigTypes.h"       // for ProcessingState
#include "engine/CDSPEngine.h"           // for CDSPEngine
#include "models/AudioDeviceManager.h"   // for AudioDeviceManager
#include "models/AudioSettings.h"        // for AudioSettings
#include "models/DSPEngineController.h"  // for DSPEngineController
#include "models/MonitoringController.h" // for MonitoringController
#include "models/PipelineStore.h"        // for PipelineStore
#include "models/SpectrogramEngine.h"    // for SpectrogramEngine
#include "models/SpectrumEngine.h"       // for SpectrumEngine
#include "models/VectorScopeEngine.h"    // for VectorScopeEngine
#include "ui/LevelMeterView.h"           // for CompactLevelMeterBar
#include "ui/MiniPlayerView.h"           // for MiniPlayerView

#include <QAction>         // for QAction
#include <QLabel>          // for QLabel
#include <QMainWindow>     // for QMainWindow
#include <QMap>            // for QMap
#include <QMenu>           // for QMenu
#include <QObject>         // for Q_OBJECT, slots
#include <QPushButton>     // for QPushButton
#include <QSlider>         // for QSlider
#include <QSplitter>       // for QSplitter
#include <QStackedWidget>  // for QStackedWidget
#include <QString>         // for QString
#include <QSystemTrayIcon> // for QSystemTrayIcon
#include <QTreeWidget>     // for QTreeWidget
#include <QTreeWidgetItem> // for QTreeWidgetItem
#include <QWidget>         // for QWidget
#include <memory>          // for shared_ptr, unique_ptr

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onSidebarItemClicked(QTreeWidgetItem* item, int column);
    void onPipelineChanged();
    void onEngineStatusChanged(ProcessingState state);
    void toggleMiniPlayer();
    void toggleMute();
    void updateMuteDisplay();
    void updateVolumeDisplay();
    void updateStatusBar();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;

    std::shared_ptr<SpectrumEngine> m_spectrumEngine;
    std::shared_ptr<SpectrogramEngine> m_spectrogramEngine;
    std::shared_ptr<VectorScopeEngine> m_vectorScopeEngine;
    std::shared_ptr<MonitoringController> m_monitoring;

    std::unique_ptr<MiniPlayerView> m_miniPlayer;

    QTreeWidget* m_sidebarTree;
    QStackedWidget* m_centralStack;
    QSplitter* m_splitter;
    CompactLevelMeterBar* m_compactMeterBar;

    QPushButton* m_startStopBtn;
    QLabel* m_sampleRateBadge;
    QPushButton* m_toolbarMuteBtn;
    QSlider* m_headerVolumeSlider;
    QLabel* m_gainValueLabel;

    // Status Bar Widgets
    QLabel* m_statusStateLabel;
    QLabel* m_stopReasonBanner;

    // Tray Icon & Menu Bar Actions
    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_trayMenu = nullptr;
    QAction* m_trayStartStopAction = nullptr;
    QAction* m_trayMuteAction = nullptr;
    QAction* m_actImportConv = nullptr;
    QAction* m_actAddEqPreset = nullptr;
    QAction* m_actRoomCorrection = nullptr;
    QAction* m_actOratoryPreset = nullptr;
    QAction* m_actAutoEqPreset = nullptr;
    QAction* m_actStartStop = nullptr;
    QAction* m_actMute = nullptr;

    QMap<QString, QWidget*> m_pageCache;
    QWidget* m_unavailableWidget = nullptr;
    QString m_lastActiveTag = "dashboard";

    void setupUi();
    void setupSidebar();
    void setupToolbar();
    void setupMenuBar();
    void setupStatusBar();
    void setupTrayIcon();
    void setupShortcuts();
    void updateTrayMenu();
    void refreshSidebarItems();
    void showCentralWidget(QWidget* widget);
    void handleNavigationTag(const QString& tag);
    void showAndActivate();
};

#endif // MAIN_WINDOW_H
