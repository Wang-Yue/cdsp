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

#include <QMainWindow>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <memory>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onSidebarItemClicked(QTreeWidgetItem* item, int column);
    void onPipelineChanged();
    void onEngineStatusChanged(ProcessingState state);
    void toggleMiniPlayer();

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

    QPushButton* m_startStopBtn;
    QLabel* m_sampleRateBadge;
    QSlider* m_headerVolumeSlider;

    QString m_lastActiveTag = "dashboard";

    void setupUi();
    void setupSidebar();
    void setupToolbar();
    void refreshSidebarItems();
    void showCentralWidget(QWidget* widget);
    void handleNavigationTag(const QString& tag);
};

#endif // MAIN_WINDOW_H
