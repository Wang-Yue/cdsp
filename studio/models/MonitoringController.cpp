#include "models/MonitoringController.h"

#include "models/DSPEngineController.h"
#include "models/LogManager.h"

MonitoringController::MonitoringController(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<LevelState> levelsPtr,
                                           std::shared_ptr<SpectrumEngine> spectrumEngine,
                                           std::shared_ptr<SpectrogramEngine> spectrogramEngine,
                                           std::shared_ptr<VectorScopeEngine> vectorScopeEngine,
                                           std::shared_ptr<AudioDeviceManager> devices,
                                           std::shared_ptr<AudioSettings> settings, QObject* parent)
    : QObject(parent), levels(levelsPtr), m_engine(engine), m_devices(devices), m_settings(settings),
      m_spectrumEngine(spectrumEngine), m_spectrogramEngine(spectrogramEngine), m_vectorScopeEngine(vectorScopeEngine) {

    if (!levels) {
        levels = std::make_shared<LevelState>();
    }

    QSettings s("DSPMonitor", "MonitorQt");
    m_showLevelMetersInDashboard = s.value("show_levels_in_dashboard", true).toBool();
    m_showSpectrumInDashboard = s.value("show_spectrum_in_dashboard", true).toBool();
    m_showSpectrogramInDashboard = s.value("show_spectrogram_in_dashboard", true).toBool();
    m_showVectorScopeInDashboard = s.value("show_vectorscope_in_dashboard", true).toBool();
    m_showAnalogVUInDashboard = s.value("show_analog_vu_in_dashboard", true).toBool();
    m_showSignalGraphInDashboard = s.value("show_signal_graph_in_dashboard", true).toBool();

    double savedPollingRate = s.value("pollingRate", 10.0).toDouble();
    m_pollingRate = savedPollingRate > 0.0 ? savedPollingRate : 10.0;

    connect(&m_pollTimer, &QTimer::timeout, this, &MonitoringController::onPollTimer);
    int intervalMs = static_cast<int>(1000.0 / std::max(1.0, m_pollingRate));
    m_pollTimer.setInterval(intervalMs);
}

MonitoringController::MonitoringController(std::shared_ptr<CDSPEngine> engine,
                                           std::shared_ptr<DSPEngineController> dspController,
                                           std::shared_ptr<SpectrumEngine> spectrumEngine,
                                           std::shared_ptr<SpectrogramEngine> spectrogramEngine,
                                           std::shared_ptr<VectorScopeEngine> vectorScopeEngine, QObject* parent)
    : MonitoringController(engine, dspController ? dspController->levels() : nullptr, spectrumEngine, spectrogramEngine,
                           vectorScopeEngine, dspController ? dspController->devices() : nullptr,
                           dspController ? dspController->settings() : nullptr, parent) {

    if (dspController) {
        onStatusChange = [dspController](ProcessingState state) {
            dspController->status = state;
            emit dspController->statusChanged(state);
        };
        onRestartEngine = [dspController]() { dspController->startEngine(); };
    }
}

void MonitoringController::start() {
    m_pollTimer.start();
}

void MonitoringController::stop() {
    m_pollTimer.stop();
}

void MonitoringController::setPollingRate(double rateHz) {
    m_pollingRate = rateHz;
    QSettings s("DSPMonitor", "MonitorQt");
    s.setValue("pollingRate", rateHz);
    int intervalMs = static_cast<int>(1000.0 / std::max(1.0, rateHz));
    m_pollTimer.setInterval(intervalMs);
}

void MonitoringController::setShowLevelMetersInDashboard(bool show) {
    m_showLevelMetersInDashboard = show;
    if (m_settings) {
        m_settings->showLevelMetersInDashboard = show;
        m_settings->savePreferences();
    } else {
        QSettings("DSPMonitor", "MonitorQt").setValue("show_levels_in_dashboard", show);
    }
    emit dashboardVisibilityChanged();
}

void MonitoringController::setShowSpectrumInDashboard(bool show) {
    m_showSpectrumInDashboard = show;
    if (m_settings) {
        m_settings->showSpectrumInDashboard = show;
        m_settings->savePreferences();
    } else {
        QSettings("DSPMonitor", "MonitorQt").setValue("show_spectrum_in_dashboard", show);
    }
    emit dashboardVisibilityChanged();
}

void MonitoringController::setShowSpectrogramInDashboard(bool show) {
    m_showSpectrogramInDashboard = show;
    if (m_settings) {
        m_settings->showSpectrogramInDashboard = show;
        m_settings->savePreferences();
    } else {
        QSettings("DSPMonitor", "MonitorQt").setValue("show_spectrogram_in_dashboard", show);
    }
    emit dashboardVisibilityChanged();
}

void MonitoringController::setShowVectorScopeInDashboard(bool show) {
    m_showVectorScopeInDashboard = show;
    if (m_settings) {
        m_settings->showVectorScopeInDashboard = show;
        m_settings->savePreferences();
    } else {
        QSettings("DSPMonitor", "MonitorQt").setValue("show_vectorscope_in_dashboard", show);
    }
    emit dashboardVisibilityChanged();
}

void MonitoringController::setShowAnalogVUInDashboard(bool show) {
    m_showAnalogVUInDashboard = show;
    if (m_settings) {
        m_settings->showAnalogVUInDashboard = show;
        m_settings->savePreferences();
    } else {
        QSettings("DSPMonitor", "MonitorQt").setValue("show_analog_vu_in_dashboard", show);
    }
    emit dashboardVisibilityChanged();
}

void MonitoringController::setShowSignalGraphInDashboard(bool show) {
    m_showSignalGraphInDashboard = show;
    if (m_settings) {
        m_settings->showSignalGraphInDashboard = show;
        m_settings->savePreferences();
    } else {
        QSettings("DSPMonitor", "MonitorQt").setValue("show_signal_graph_in_dashboard", show);
    }
    emit dashboardVisibilityChanged();
}

void MonitoringController::onPollTimer() {
    poll();
}

void MonitoringController::poll() {
    if (!m_engine)
        return;

    m_engine->poll();

    // 1. Poll Status
    StateUpdate update = m_engine->getStatus();
    handleStateUpdate(update.state, update.stopReason);

    size_t capChannels = m_devices ? m_devices->captureConfig.channels : 2;
    size_t pbChannels = m_devices ? m_devices->playbackConfig.channels : 2;

    // 2. Poll VU Levels
    int vuVisibilityCount = (levels ? levels->visibilityCount : 0) + levelState.visibilityCount;
    if (m_currentStatus != ProcessingState::Inactive && m_currentStatus != ProcessingState::Paused &&
        vuVisibilityCount > 0) {
        VuLevels vu = m_engine->getVuLevels();
        VuLevels clampedVu;
        clampedVu.capture_peak.reserve(vu.capture_peak.size());
        for (float v : vu.capture_peak)
            clampedVu.capture_peak.push_back(std::max(-100.0f, v));
        clampedVu.capture_rms.reserve(vu.capture_rms.size());
        for (float v : vu.capture_rms)
            clampedVu.capture_rms.push_back(std::max(-100.0f, v));
        clampedVu.playback_peak.reserve(vu.playback_peak.size());
        for (float v : vu.playback_peak)
            clampedVu.playback_peak.push_back(std::max(-100.0f, v));
        clampedVu.playback_rms.reserve(vu.playback_rms.size());
        for (float v : vu.playback_rms)
            clampedVu.playback_rms.push_back(std::max(-100.0f, v));

        levels->update(clampedVu);
        levelState.update(clampedVu);
        emit levelsUpdated();
    } else {
        bool changed = false;
        if (levels)
            changed = levels->reset(capChannels, pbChannels);
        bool lsChanged = levelState.reset(capChannels, pbChannels);
        if (changed || lsChanged)
            emit levelsUpdated();
    }

    // 3. Poll Spectrum
    if (m_currentStatus != ProcessingState::Inactive && m_currentStatus != ProcessingState::Paused &&
        m_spectrumEngine && m_spectrumEngine->visibilityCount > 0) {
        SpectrumData specData;
        int specCh = m_spectrumEngine->channel.value_or(-1);
        if (m_engine->getSpectrum(m_spectrumEngine->isCapture, specCh, m_spectrumEngine->minFreq,
                                  m_spectrumEngine->maxFreq, m_spectrumEngine->nBins, specData)) {
            m_spectrumEngine->update(specData);
        } else {
            m_spectrumEngine->reset();
        }
    } else if (m_spectrumEngine) {
        m_spectrumEngine->reset();
    }

    // 4. Poll Spectrogram
    if (m_currentStatus != ProcessingState::Inactive && m_currentStatus != ProcessingState::Paused &&
        m_spectrogramEngine && m_spectrogramEngine->visibilityCount > 0) {
        SpectrumData spectroData;
        int spectroCh = m_spectrogramEngine->channel.value_or(-1);
        if (m_engine->getSpectrum(m_spectrogramEngine->isCapture, spectroCh, m_spectrogramEngine->minFreq,
                                  m_spectrogramEngine->maxFreq, m_spectrogramEngine->nBins, spectroData)) {
            m_spectrogramEngine->pushSpectrum(spectroData);
        } else {
            m_spectrogramEngine->reset();
        }
    } else if (m_spectrogramEngine) {
        m_spectrogramEngine->reset();
    }

    // 5. Poll Vector Scope
    if (m_currentStatus != ProcessingState::Inactive && m_currentStatus != ProcessingState::Paused &&
        m_vectorScopeEngine && m_vectorScopeEngine->visibilityCount > 0) {
        AudioSamplesData samples;
        if (m_engine->getSamples(m_vectorScopeEngine->isCapture, m_vectorScopeEngine->nFrames, samples)) {
            m_vectorScopeEngine->update(samples);
        } else {
            m_vectorScopeEngine->reset();
        }
    } else if (m_vectorScopeEngine) {
        m_vectorScopeEngine->reset();
    }
}

void MonitoringController::handleStateUpdate(ProcessingState state, const ProcessingStopReason& stopReason) {
    if (state != m_currentStatus) {
        m_currentStatus = state;
        if (onStatusChange) {
            onStatusChange(state);
        }
        emit statusChanged(state);
        if (state == ProcessingState::Inactive || state == ProcessingState::Paused) {
            size_t capCh = m_devices ? m_devices->captureConfig.channels : 2;
            size_t pbCh = m_devices ? m_devices->playbackConfig.channels : 2;
            bool changed = false;
            if (levels)
                changed = levels->reset(capCh, pbCh);
            bool lsChanged = levelState.reset(capCh, pbCh);
            if (changed || lsChanged)
                emit levelsUpdated();
            if (m_spectrumEngine)
                m_spectrumEngine->reset();
            if (m_spectrogramEngine)
                m_spectrogramEngine->reset();
            if (m_vectorScopeEngine)
                m_vectorScopeEngine->reset();
        }
    }

    switch (stopReason.type) {
    case StopReasonType::None:
    case StopReasonType::Done:
        break;
    case StopReasonType::CaptureError:
        LogManager::instance()->appendLog(
            LogLevel::Error, QString::fromStdString("[MonitoringController] Capture error: " + stopReason.message));
        break;
    case StopReasonType::PlaybackError:
        LogManager::instance()->appendLog(
            LogLevel::Error, QString::fromStdString("[MonitoringController] Playback error: " + stopReason.message));
        break;
    case StopReasonType::CaptureFormatChange: {
        int newRate = stopReason.formatChangeRate;
        LogManager::instance()->appendLog(
            LogLevel::Warn,
            QString("[MonitoringController] Capture format change detected, switching to %1 Hz").arg(newRate));
        if (m_settings && m_settings->resamplerEnabled) {
            if (m_devices) {
                DeviceConfig newCfg = m_devices->captureConfig;
                newCfg.sampleRate = newRate;
                m_devices->setCaptureConfig(newCfg);
            }
        } else {
            if (m_devices) {
                DeviceConfig newCfg = m_devices->playbackConfig;
                newCfg.sampleRate = newRate;
                m_devices->setPlaybackConfig(newCfg);
            }
        }
        if (onRestartEngine) {
            onRestartEngine();
        }
        emit restartEngineRequested();
        break;
    }
    case StopReasonType::PlaybackFormatChange: {
        int newRate = stopReason.formatChangeRate;
        LogManager::instance()->appendLog(
            LogLevel::Warn,
            QString("[MonitoringController] Playback format change detected, switching to %1 Hz").arg(newRate));
        if (m_devices) {
            DeviceConfig newCfg = m_devices->playbackConfig;
            newCfg.sampleRate = newRate;
            m_devices->setPlaybackConfig(newCfg);
        }
        if (onRestartEngine) {
            onRestartEngine();
        }
        emit restartEngineRequested();
        break;
    }
    case StopReasonType::UnknownError:
        LogManager::instance()->appendLog(
            LogLevel::Error, QString::fromStdString("[MonitoringController] Unknown error: " + stopReason.message));
        break;
    }
}
