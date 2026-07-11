#include "models/DSPEngineController.h"
#include "models/LogManager.h"

DSPEngineController::DSPEngineController(
    std::shared_ptr<CDSPEngine> engine,
    std::shared_ptr<AudioDeviceManager> devices,
    std::shared_ptr<AudioSettings> settings,
    std::shared_ptr<PipelineStore> pipeline,
    QObject* parent
) : QObject(parent), m_engine(engine), m_devices(devices), m_settings(settings), m_pipeline(pipeline) {

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        LogManager::instance()->appendLog(LogLevel::Info, QString("Executing scheduled auto-restart attempt %1...").arg(m_retryCount));
        startEngine();
    });

    connect(m_devices.get(), &AudioDeviceManager::configChanged, this, [this]() {
        if (status == ProcessingState::Running) {
            applyConfig();
        }
    });

    connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, [this]() {
        if (status == ProcessingState::Running) {
            applyConfig();
        }
    });
}

DSPConfiguration DSPEngineController::buildConfiguration() const {
    DSPConfiguration config;

    int captureRate = m_devices->captureConfig.sampleRate;
    int playbackRate = m_devices->playbackConfig.sampleRate;
    int channelCount = m_devices->captureConfig.channels;

    config.devices.samplerate = playbackRate;
    config.devices.chunksize = m_settings->chunkSize;
    if (m_settings->enableRateAdjust) {
        config.devices.enableRateAdjust = true;
    }

    if (m_settings->queuelimit > 0) config.devices.queuelimit = m_settings->queuelimit;
    if (m_settings->stopOnRateChange) config.devices.stopOnRateChange = m_settings->stopOnRateChange;
    if (m_settings->rateMeasureInterval > 0) config.devices.rateMeasureInterval = m_settings->rateMeasureInterval;
    if (m_settings->multithreaded) config.devices.multithreaded = m_settings->multithreaded;
    if (m_settings->multithreaded && m_settings->workerThreads > 0) config.devices.workerThreads = m_settings->workerThreads;
    if (m_settings->silenceTimeout > 0) {
        config.devices.silenceThreshold = static_cast<double>(m_settings->silenceThreshold);
        config.devices.silenceTimeout = static_cast<double>(m_settings->silenceTimeout);
    }

    if (m_settings->resamplerEnabled) {
        config.devices.captureSamplerate = captureRate;
        ResamplerConfig resCfg;
        resCfg.type = m_settings->resamplerType;
        switch (m_settings->resamplerType) {
        case ResamplerType::AsyncSinc:
            if (m_settings->resamplerUseProfile) {
                resCfg.profile = resamplerProfileToString(m_settings->resamplerProfile);
            } else {
                resCfg.sincLen = m_settings->resamplerSincLen;
                resCfg.oversamplingFactor = m_settings->resamplerOversamplingFactor;
                resCfg.window = m_settings->resamplerWindow;
                resCfg.fCutoff = m_settings->resamplerFCutoff;
                switch (m_settings->resamplerSincInterpolation) {
                case SincInterpolation::Nearest: resCfg.interpolation = "Nearest"; break;
                case SincInterpolation::Linear: resCfg.interpolation = "Linear"; break;
                case SincInterpolation::Quadratic: resCfg.interpolation = "Quadratic"; break;
                case SincInterpolation::Cubic: resCfg.interpolation = "Cubic"; break;
                }
            }
            break;
        case ResamplerType::AsyncPoly:
            switch (m_settings->resamplerInterpolation) {
            case ResamplerInterpolation::Linear: resCfg.interpolation = "Linear"; break;
            case ResamplerInterpolation::Cubic: resCfg.interpolation = "Cubic"; break;
            case ResamplerInterpolation::Quintic: resCfg.interpolation = "Quintic"; break;
            case ResamplerInterpolation::Septic: resCfg.interpolation = "Septic"; break;
            }
            break;
        case ResamplerType::Synchronous:
            break;
        case ResamplerType::Apple:
            resCfg.appleQuality = m_settings->resamplerAppleQuality;
            resCfg.appleComplexity = m_settings->resamplerAppleComplexity;
            break;
        }
        config.devices.resampler = resCfg;
    }

    config.devices.capture = m_devices->captureConfig.toCaptureDeviceConfig();
    config.devices.playback = m_devices->playbackConfig.toPlaybackDeviceConfig();

    auto buildRes = m_pipeline->buildPipeline(captureRate, channelCount);
    config.filters = buildRes.filters;
    config.mixers = buildRes.mixers;
    config.processors = buildRes.processors;
    config.pipeline = buildRes.steps;

    return config;
}

void DSPEngineController::startEngine() {
    m_userStopped = false;
    status = ProcessingState::Starting;
    emit statusChanged(status);

    // Prime faders BEFORE starting config to prevent 0 dBFS jumps/pops
    syncFaders();

    DSPConfiguration config = buildConfiguration();
    std::string jsonStr = config.toJsonString();

    std::string err;
    bool ok = m_engine->start(jsonStr, err);
    if (ok) {
        status = ProcessingState::Running;
        m_lastStartTime = QDateTime::currentDateTime();
        syncFaders();
        LogManager::instance()->appendLog(LogLevel::Info, "DSP Engine started successfully.");
    } else {
        status = ProcessingState::Inactive;
        lastErrorMessage = err;
        LogManager::instance()->appendLog(LogLevel::Error, QString::fromStdString("Engine start failed: " + err));
        if (!m_userStopped) {
            scheduleAutoRestart(1000);
        }
    }
    emit statusChanged(status);
}

void DSPEngineController::stopEngine() {
    m_userStopped = true;
    m_reconnectTimer.stop();
    m_retryCount = 0;
    m_engine->stop();
    status = ProcessingState::Inactive;
    emit statusChanged(status);
    LogManager::instance()->appendLog(LogLevel::Info, "DSP Engine stopped by user.");
}

void DSPEngineController::applyConfig() {
    if (status != ProcessingState::Running) return;
    syncFaders();
    DSPConfiguration config = buildConfiguration();
    std::string jsonStr = config.toJsonString();

    std::string err;
    bool ok = m_engine->start(jsonStr, err);
    if (ok) {
        syncFaders();
        emit configApplied();
        LogManager::instance()->appendLog(LogLevel::Info, "Dynamic configuration applied successfully.");
    } else {
        lastErrorMessage = err;
        LogManager::instance()->appendLog(LogLevel::Error, QString::fromStdString("Apply config failed: " + err));
        if (!m_userStopped) {
            scheduleAutoRestart(1000);
        }
    }
}

void DSPEngineController::setFaderVolume(Fader fader, float db, bool instant) {
    m_settings->setVolume(db, fader);
    // instant = false activates smooth volume ramping to avoid clicks/pops
    m_engine->setFaderVolume(fader, db, instant);
}

void DSPEngineController::setFaderMute(Fader fader, bool mute) {
    m_settings->setMuted(mute, fader);
    m_engine->setFaderMute(fader, mute);
}

void DSPEngineController::syncFaders() {
    for (Fader f : {Fader::Main, Fader::Aux1, Fader::Aux2, Fader::Aux3, Fader::Aux4}) {
        m_engine->setFaderVolume(f, m_settings->getVolume(f), true);
        m_engine->setFaderMute(f, m_settings->getMuted(f));
    }
}

void DSPEngineController::scheduleAutoRestart(int baseDelayMs) {
    if (m_userStopped || m_reconnectTimer.isActive()) return;
    if (m_retryCount >= m_maxRetries) {
        LogManager::instance()->appendLog(LogLevel::Error, QString("Engine auto-restart exceeded maximum retry count (%1 attempts). Giving up.").arg(m_maxRetries));
        return;
    }

    int delayMs = baseDelayMs * (1 << m_retryCount);
    if (delayMs > 16000) delayMs = 16000;

    m_retryCount++;
    LogManager::instance()->appendLog(LogLevel::Warn, QString("Engine crash/stop detected. Scheduling auto-restart attempt %1/%2 in %3 ms...")
        .arg(m_retryCount).arg(m_maxRetries).arg(delayMs));

    m_reconnectTimer.start(delayMs);
}

void DSPEngineController::updateStatus(const StateUpdate& update) {
    bool stateChanged = (status != update.state);
    status = update.state;
    lastStopReason = update.stopReason;

    if (stateChanged) {
        emit statusChanged(status);
    }
    emit statusUpdated(status, lastStopReason);

    if (status == ProcessingState::Running) {
        if (m_lastStartTime.isValid() && m_lastStartTime.secsTo(QDateTime::currentDateTime()) > 5) {
            m_retryCount = 0; // Reset retry counter after stable execution
        }
    }

    if (update.stopReason.type == StopReasonType::CaptureFormatChange || update.stopReason.type == StopReasonType::PlaybackFormatChange) {
        int newRate = update.stopReason.formatChangeRate;
        if (newRate > 0) {
            LogManager::instance()->appendLog(LogLevel::Warn, QString("Format change detected (%1 Hz), restarting engine immediately...").arg(newRate));
            m_devices->captureConfig.sampleRate = newRate;
            m_devices->playbackConfig.sampleRate = newRate;
            m_devices->saveConfigs();
            scheduleAutoRestart(0);
        }
    } else if (!m_userStopped && (update.stopReason.type == StopReasonType::CaptureError ||
                                  update.stopReason.type == StopReasonType::PlaybackError ||
                                  update.stopReason.type == StopReasonType::UnknownError ||
                                  status == ProcessingState::Inactive ||
                                  status == ProcessingState::Stalled)) {
        scheduleAutoRestart(1000);
    }
}
