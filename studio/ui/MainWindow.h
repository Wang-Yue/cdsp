#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "engine/CDSPEngine.h"
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include "models/PipelineStore.h"
#include "models/DSPEngineController.h"
#include "models/MonitoringController.h"
#include "models/SpectrumEngine.h"
#include "models/SpectrogramEngine.h"
#include "models/VectorScopeEngine.h"
#include "ui/MiniPlayerView.h"
#include "ui/LevelMeterView.h"

#include <QMainWindow>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QSystemTrayIcon>
#include <QSplitter>
#include <QStatusBar>
#include <QAction>
#include <QKeySequence>

#include <QElapsedTimer>
#include <QTimer>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

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
    QLabel* m_statusActivePresetLabel;
    QLabel* m_statusRuntimeLabel;
    QLabel* m_statusMuteLabel;
    QLabel* m_stopReasonBanner;

    QElapsedTimer m_engineRunTimer;
    QTimer m_runtimeUpdateTimer;

    // Tray Icon
    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayMenu;

    QMap<QString, QWidget*> m_pageCache;
    QString m_lastActiveTag = "dashboard";

    void setupUi();
    void setupSidebar();
    void setupToolbar();
    void setupStatusBar();
    void setupTrayIcon();
    void setupShortcuts();
    void refreshSidebarItems();
    void showCentralWidget(QWidget* widget);
    void handleNavigationTag(const QString& tag);
};

#endif // MAIN_WINDOW_H
