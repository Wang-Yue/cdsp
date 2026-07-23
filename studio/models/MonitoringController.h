#ifndef MONITORING_CONTROLLER_H
#define MONITORING_CONTROLLER_H

#include "engine/CDSPEngine.h"
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include "models/LevelState.h"
#include "models/SpectrogramEngine.h"
#include "models/SpectrumEngine.h"
#include "models/VectorScopeEngine.h"

#include <QObject>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <memory>

class DSPEngineController;

class MonitoringController : public QObject {
    Q_OBJECT

public:
    MonitoringController(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<LevelState> levels,
                         std::shared_ptr<SpectrumEngine> spectrumEngine,
                         std::shared_ptr<SpectrogramEngine> spectrogramEngine,
                         std::shared_ptr<VectorScopeEngine> vectorScopeEngine,
                         std::shared_ptr<AudioDeviceManager> devices, std::shared_ptr<AudioSettings> settings,
                         QObject* parent = nullptr);

    // Overload for backward compatibility
    MonitoringController(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<DSPEngineController> dspController,
                         std::shared_ptr<SpectrumEngine> spectrumEngine,
                         std::shared_ptr<SpectrogramEngine> spectrogramEngine,
                         std::shared_ptr<VectorScopeEngine> vectorScopeEngine, QObject* parent = nullptr);

    std::shared_ptr<LevelState> levels;
    LevelState levelState; // Backward compatibility for UI views

    bool isMiniPlayerActive = false;

    bool showLevelMetersInDashboard() const {
        return m_settings ? m_settings->showLevelMetersInDashboard : m_showLevelMetersInDashboard;
    }
    void setShowLevelMetersInDashboard(bool show);

    bool showSpectrumInDashboard() const {
        return m_settings ? m_settings->showSpectrumInDashboard : m_showSpectrumInDashboard;
    }
    void setShowSpectrumInDashboard(bool show);

    bool showSpectrogramInDashboard() const {
        return m_settings ? m_settings->showSpectrogramInDashboard : m_showSpectrogramInDashboard;
    }
    void setShowSpectrogramInDashboard(bool show);

    bool showVectorScopeInDashboard() const {
        return m_settings ? m_settings->showVectorScopeInDashboard : m_showVectorScopeInDashboard;
    }
    void setShowVectorScopeInDashboard(bool show);

    bool showAnalogVUInDashboard() const {
        return m_settings ? m_settings->showAnalogVUInDashboard : m_showAnalogVUInDashboard;
    }
    void setShowAnalogVUInDashboard(bool show);

    bool showSignalGraphInDashboard() const {
        return m_settings ? m_settings->showSignalGraphInDashboard : m_showSignalGraphInDashboard;
    }
    void setShowSignalGraphInDashboard(bool show);

    bool m_showLevelMetersInDashboard = true;
    bool m_showSpectrumInDashboard = true;
    bool m_showSpectrogramInDashboard = true;
    bool m_showVectorScopeInDashboard = true;
    bool m_showAnalogVUInDashboard = true;
    bool m_showSignalGraphInDashboard = true;

    void start();
    void stop();

    double pollingRate() const { return m_pollingRate; }
    void setPollingRate(double rateHz);

    std::shared_ptr<SpectrumEngine> spectrumEngine() const { return m_spectrumEngine; }
    std::shared_ptr<SpectrogramEngine> spectrogramEngine() const { return m_spectrogramEngine; }
    std::shared_ptr<VectorScopeEngine> vectorScopeEngine() const { return m_vectorScopeEngine; }

    /// Fired with the new ProcessingState whenever DSP engine reports a state change.
    std::function<void(ProcessingState)> onStatusChange;
    /// Fired with ProcessingState and ProcessingStopReason when state or stop reason updates.
    std::function<void(ProcessingState, const ProcessingStopReason&)> onStatusUpdated;
    /// Fired when a CaptureFormatChange or PlaybackFormatChange stop reason requires restarting the engine.
    std::function<void()> onRestartEngine;

    ProcessingState currentStatus() const { return m_currentStatus; }

signals:
    void levelsUpdated();
    void statusChanged(ProcessingState state);
    void restartEngineRequested();
    void dashboardVisibilityChanged();

private slots:
    void onPollTimer();

private:
    void poll();
    void handleStateUpdate(ProcessingState state, const ProcessingStopReason& stopReason);

    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<SpectrumEngine> m_spectrumEngine;
    std::shared_ptr<SpectrogramEngine> m_spectrogramEngine;
    std::shared_ptr<VectorScopeEngine> m_vectorScopeEngine;

    QTimer m_pollTimer;
    double m_pollingRate = 10.0;
    ProcessingState m_currentStatus = ProcessingState::Inactive;
    ProcessingStopReason m_lastStopReason;
};

#endif // MONITORING_CONTROLLER_H
