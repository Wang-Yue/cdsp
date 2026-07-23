#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "engine/CDSPEngine.h"
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include "models/DSPEngineController.h"
#include "models/MonitoringController.h"
#include "models/PipelineStore.h"
#include "models/SpectrogramEngine.h"
#include "models/SpectrumEngine.h"
#include "models/VectorScopeEngine.h"
#include "ui/LevelMeterView.h"
#include "ui/MiniPlayerView.h"

#include <QAction>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTreeWidget>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

protected:
    void changeEvent(QEvent* event) override;

private slots:
    void onSidebarItemClicked(QTreeWidgetItem* item, int column);
    void onPipelineChanged();
    void onEngineStatusChanged(ProcessingState state);
    void toggleMiniPlayer();
    void toggleMute();
    void updateTheme();
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
    QLabel* m_statusSampleRateBadge;
    QLabel* m_statusBufferLabel;
    QLabel* m_statusResamplerLabel;
    QLabel* m_statusStagesLabel;
    QLabel* m_statusActivePresetLabel;
    QLabel* m_statusRuntimeLabel;
    QLabel* m_statusMuteLabel;
    QLabel* m_stopReasonBanner;

    QElapsedTimer m_engineRunTimer;
    QTimer m_runtimeUpdateTimer;

    // Tray Icon & Menu Bar Actions
    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayMenu;
    QMenu* m_trayPresetSubMenu = nullptr;
    QAction* m_trayMuteAction = nullptr;
    QAction* m_actImportConv = nullptr;
    QAction* m_actAddEqPreset = nullptr;
    QAction* m_actRoomCorrection = nullptr;
    QAction* m_actOratoryPreset = nullptr;
    QAction* m_actAutoEqPreset = nullptr;
    QAction* m_actStartStop = nullptr;
    QAction* m_actMute = nullptr;
    QAction* m_actThemeLight = nullptr;
    QAction* m_actThemeDark = nullptr;

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
    void selectActiveEQPreset(const QUuid& presetId);
    void refreshSidebarItems();
    void showCentralWidget(QWidget* widget);
    void handleNavigationTag(const QString& tag);
};

#endif // MAIN_WINDOW_H
